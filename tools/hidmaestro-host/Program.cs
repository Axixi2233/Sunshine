using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Diagnostics.CodeAnalysis;
using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using HIDMaestro;

namespace Sunshine.HIDMaestroHost;

/// <summary>Protocol constants shared with Sunshine's native client.</summary>
internal static class Protocol
{
    internal const uint Magic = 0x314D4853;
    internal const ushort Version = 1;
    internal const int HeaderSize = 16;
    internal const int MaxPayloadSize = 64 * 1024;

    internal enum MessageType : ushort
    {
        Ready = 1,
        CreateController = 2,
        ControllerCreated = 3,
        DestroyController = 4,
        GamepadState = 5,
        Touch = 6,
        Motion = 7,
        Battery = 8,
        Rumble = 9,
        Rgb = 10,
        AdaptiveTriggers = 11,
        Log = 12,
        Shutdown = 13,
        PlayerIndicator = 14,
        ControllerPcm = 15,
    }
}

/// <summary>One owned native controller PCM window waiting for pipe delivery.</summary>
internal sealed record NativePcmFrame(uint ControllerId, ushort Sequence, byte Channels, byte BitsPerSample, uint SampleRate, byte[] Pcm);

/// <summary>Aggregates low-rate diagnostics for native DualSense PCM sent through Sunshine.</summary>
internal sealed class NativePcmDiagnosticWindow
{
    private static readonly TimeSpan ReportInterval = TimeSpan.FromSeconds(2);
    private long reportStarted = Stopwatch.GetTimestamp();

    internal long Windows { get; private set; }
    internal long Bytes { get; private set; }
    internal long Dropped { get; private set; }
    internal int[] Peaks { get; } = new int[4];

    /// <summary>Add one transmitted PCM window and any host-side drops observed before it.</summary>
    /// <param name="frame">PCM window transmitted to Sunshine.</param>
    /// <param name="dropped">Number of stale windows discarded since the previous transmission.</param>
    internal void Add(NativePcmFrame frame, int dropped)
    {
        Windows++;
        Bytes += frame.Pcm.Length;
        Dropped += dropped;
        AccumulatePeaks(frame.Pcm, frame.Channels, frame.BitsPerSample, Peaks);
    }

    /// <summary>Determine whether this diagnostic window has accumulated for long enough.</summary>
    /// <returns>True when the current values should be logged.</returns>
    internal bool ShouldReport()
    {
        return Stopwatch.GetElapsedTime(reportStarted) >= ReportInterval;
    }

    /// <summary>Reset counters after one diagnostic report.</summary>
    internal void Reset()
    {
        Windows = 0;
        Bytes = 0;
        Dropped = 0;
        Array.Clear(Peaks);
        reportStarted = Stopwatch.GetTimestamp();
    }

    /// <summary>Accumulate absolute per-channel peaks from interleaved signed PCM.</summary>
    /// <param name="pcm">Interleaved PCM bytes.</param>
    /// <param name="channels">Number of interleaved channels.</param>
    /// <param name="bitsPerSample">Bits stored for each sample.</param>
    /// <param name="peaks">Four-element destination containing absolute channel peaks.</param>
    internal static void AccumulatePeaks(ReadOnlySpan<byte> pcm, int channels, int bitsPerSample, Span<int> peaks)
    {
        if (channels <= 0 || bitsPerSample != 16 || peaks.Length < 4)
        {
            return;
        }

        int frameBytes = channels * sizeof(short);
        for (int frameOffset = 0; frameOffset + frameBytes <= pcm.Length; frameOffset += frameBytes)
        {
            int reportedChannels = Math.Min(channels, 4);
            for (int channel = 0; channel < reportedChannels; channel++)
            {
                short sample = BinaryPrimitives.ReadInt16LittleEndian(pcm.Slice(frameOffset + channel * sizeof(short), sizeof(short)));
                peaks[channel] = Math.Max(peaks[channel], Math.Abs((int)sample));
            }
        }
    }
}

/// <summary>Encodes the data portion of a DualSense USB input report 0x01.</summary>
internal static class DualSenseInputReport
{
    internal const int DataLength = 63;

    /// <summary>Encode one complete report without the report-ID byte.</summary>
    /// <param name="state">Current logical controller state.</param>
    /// <param name="leftStickX">Normalized left-stick X axis.</param>
    /// <param name="leftStickY">Normalized left-stick Y axis.</param>
    /// <param name="rightStickX">Normalized right-stick X axis.</param>
    /// <param name="rightStickY">Normalized right-stick Y axis.</param>
    /// <param name="leftTrigger">Normalized left-trigger axis.</param>
    /// <param name="rightTrigger">Normalized right-trigger axis.</param>
    /// <param name="sequence">Rolling USB report sequence number.</param>
    /// <param name="report">Destination for the 63 data bytes.</param>
    internal static void Encode(
        in HMGamepadState state,
        float leftStickX,
        float leftStickY,
        float rightStickX,
        float rightStickY,
        float leftTrigger,
        float rightTrigger,
        ref byte sequence,
        Span<byte> report)
    {
        if (report.Length < DataLength)
        {
            throw new ArgumentException($"DualSense input report requires {DataLength} data bytes.", nameof(report));
        }

        report[..DataLength].Clear();
        report[0] = EncodeAxis(leftStickX);
        report[1] = EncodeAxis(leftStickY);
        report[2] = EncodeAxis(rightStickX);
        report[3] = EncodeAxis(rightStickY);
        report[4] = EncodeAxis(leftTrigger);
        report[5] = EncodeAxis(rightTrigger);
        report[6] = sequence++;

        uint buttons = (uint)state.Buttons;
        byte hat = state.Hat == HMHat.None ? (byte)8 : (byte)(((int)state.Hat - 1) & 0x0F);
        report[7] = (byte)(hat |
            (IsPressed(buttons, HMButton.X) ? 1 << 4 : 0) |
            (IsPressed(buttons, HMButton.A) ? 1 << 5 : 0) |
            (IsPressed(buttons, HMButton.B) ? 1 << 6 : 0) |
            (IsPressed(buttons, HMButton.Y) ? 1 << 7 : 0));
        report[8] = (byte)(
            (IsPressed(buttons, HMButton.LeftBumper) ? 1 << 0 : 0) |
            (IsPressed(buttons, HMButton.RightBumper) ? 1 << 1 : 0) |
            (leftTrigger > 0.0f ? 1 << 2 : 0) |
            (rightTrigger > 0.0f ? 1 << 3 : 0) |
            (IsPressed(buttons, HMButton.Back) ? 1 << 4 : 0) |
            (IsPressed(buttons, HMButton.Start) ? 1 << 5 : 0) |
            (IsPressed(buttons, HMButton.LeftStick) ? 1 << 6 : 0) |
            (IsPressed(buttons, HMButton.RightStick) ? 1 << 7 : 0));
        report[9] = (byte)(
            (IsPressed(buttons, HMButton.Guide) ? 1 << 0 : 0) |
            (IsPressed(buttons, HMButton.Touchpad) ? 1 << 1 : 0) |
            (IsPressed(buttons, HMButton.Misc1) ? 1 << 2 : 0));

        BinaryPrimitives.WriteInt16LittleEndian(report[15..], state.GyroPitch);
        BinaryPrimitives.WriteInt16LittleEndian(report[17..], state.GyroYaw);
        BinaryPrimitives.WriteInt16LittleEndian(report[19..], state.GyroRoll);
        BinaryPrimitives.WriteInt16LittleEndian(report[21..], state.AccelX);
        BinaryPrimitives.WriteInt16LittleEndian(report[23..], state.AccelY);
        BinaryPrimitives.WriteInt16LittleEndian(report[25..], state.AccelZ);
        BinaryPrimitives.WriteUInt32LittleEndian(report[27..], state.SensorTimestamp);
        EncodeFinger(
            state.TouchpadFinger0Active,
            state.TouchpadFinger0Id,
            state.TouchpadFinger0X,
            state.TouchpadFinger0Y,
            report[32..36]);
        EncodeFinger(
            state.TouchpadFinger1Active,
            state.TouchpadFinger1Id,
            state.TouchpadFinger1X,
            state.TouchpadFinger1Y,
            report[36..40]);
        report[52] = (byte)((state.BatteryLevel & 0x0F) |
            (state.BatteryCharging ? 1 << 4 : 0) |
            (state.BatteryFull ? 1 << 5 : 0));
    }

    private static byte EncodeAxis(float value)
    {
        return (byte)Math.Clamp((int)Math.Round(Math.Clamp(value, 0.0f, 1.0f) * byte.MaxValue), byte.MinValue, byte.MaxValue);
    }

    private static void EncodeFinger(bool active, byte id, ushort x, ushort y, Span<byte> destination)
    {
        destination[0] = (byte)((id & 0x7F) | (active ? 0x00 : 0x80));
        destination[1] = (byte)(x & 0xFF);
        destination[2] = (byte)(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
        destination[3] = (byte)((y >> 4) & 0xFF);
    }

    private static bool IsPressed(uint buttons, HMButton button)
    {
        return (buttons & (uint)button) != 0;
    }
}

/// <summary>Mutable state and ownership for one virtual DualSense.</summary>
internal sealed class ControllerSlot : IDisposable
{
    private readonly Dictionary<uint, int> pointerSlots = new();
    private readonly byte[] trackingIds = new byte[2];
    private readonly byte[] inputReport = new byte[DualSenseInputReport.DataLength];
    private readonly Action<ControllerSlot, HMOutputPacket> outputHandler;
    private readonly Action<HMAudioOutput, ReadOnlyMemory<byte>>? pcmHandler;
    private readonly Action<HMAudioOutput, bool>? pcmStreamingHandler;
    private byte sequence;
    private ushort pcmSequence;

    internal ControllerSlot(
        HMController controller,
        HMProfile profile,
        uint id,
        Action<ControllerSlot, HMOutputPacket> outputHandler,
        Action<ControllerSlot, HMAudioOutput, ReadOnlyMemory<byte>> nativePcmHandler,
        Action<ControllerSlot, HMAudioOutput, bool> nativePcmStreamingHandler)
    {
        Controller = controller;
        Id = id;
        this.outputHandler = outputHandler;
        Axes = new Dictionary<HMAxis, float>();
        State = new HMGamepadState
        {
            Axes = Axes,
            Hat = HMHat.None,
            AccelY = 8192,
            BatteryLevel = 5,
        };

        if (profile.Sticks.Count > 0)
        {
            LeftX = profile.Sticks[0].XAxis;
            LeftY = profile.Sticks[0].YAxis;
        }
        if (profile.Sticks.Count > 1)
        {
            RightX = profile.Sticks[1].XAxis;
            RightY = profile.Sticks[1].YAxis;
        }
        if (profile.Triggers.Count > 0)
        {
            LeftTrigger = profile.Triggers[0].Axis;
        }
        if (profile.Triggers.Count > 1)
        {
            RightTrigger = profile.Triggers[1].Axis;
        }

        SetAxes(0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.0f);
        Controller.OutputReceived += OnOutputReceived;
        if (Controller.UsbAudio is not null)
        {
            pcmHandler = (output, pcm) => nativePcmHandler(this, output, pcm);
            pcmStreamingHandler = (output, streaming) => nativePcmStreamingHandler(this, output, streaming);
            Controller.UsbAudio.Output.FramesReceived += pcmHandler;
            Controller.UsbAudio.Output.StreamingChanged += pcmStreamingHandler;
        }
    }

    internal uint Id { get; }
    internal HMController Controller { get; }
    internal HMGamepadState State;
    internal Dictionary<HMAxis, float> Axes { get; }
    internal HMAxis LeftX { get; }
    internal HMAxis LeftY { get; }
    internal HMAxis RightX { get; }
    internal HMAxis RightY { get; }
    internal HMAxis LeftTrigger { get; }
    internal HMAxis RightTrigger { get; }
    internal bool LoggedRumbleOutput { get; set; }
    internal bool LoggedAdaptiveTriggerOutput { get; set; }
    internal bool LoggedRgbOutput { get; set; }
    internal bool LoggedPlayerIndicatorOutput { get; set; }
    internal bool LoggedTouchInput { get; set; }
    internal bool LoggedAccelerometerInput { get; set; }
    internal bool LoggedGyroscopeInput { get; set; }
    internal bool LoggedUnsupportedOutput { get; set; }
    internal bool LoggedInputReportFailure { get; set; }

    /// <summary>Return and advance the rolling native PCM sequence number.</summary>
    internal ushort NextPcmSequence()
    {
        return pcmSequence++;
    }

    /// <summary>Update all standard analog axes without allocating.</summary>
    internal void SetAxes(float lx, float ly, float rx, float ry, float lt, float rt)
    {
        SetAxis(LeftX, lx);
        SetAxis(LeftY, ly);
        SetAxis(RightX, rx);
        SetAxis(RightY, ry);
        SetAxis(LeftTrigger, lt);
        SetAxis(RightTrigger, rt);
    }

    /// <summary>Submit the full native DualSense USB input state.</summary>
    /// <param name="sensorTimestamp">Current emulated sensor timestamp in microseconds.</param>
    internal void Submit(uint sensorTimestamp)
    {
        State.SensorTimestamp = sensorTimestamp;
        DualSenseInputReport.Encode(
            in State,
            GetAxis(LeftX, 0.5f),
            GetAxis(LeftY, 0.5f),
            GetAxis(RightX, 0.5f),
            GetAxis(RightY, 0.5f),
            GetAxis(LeftTrigger, 0.0f),
            GetAxis(RightTrigger, 0.0f),
            ref sequence,
            inputReport);
        Controller.SubmitRawReport(inputReport);
    }

    /// <summary>Apply one Moonlight touch event to the two Sony contacts.</summary>
    internal void ApplyTouch(uint pointerId, byte eventType, float x, float y)
    {
        if (eventType == 7)
        {
            pointerSlots.Clear();
            State.TouchpadFinger0Active = false;
            State.TouchpadFinger1Active = false;
            State.TouchpadPacketCounter++;
            return;
        }

        int finger;
        if (eventType == 1)
        {
            if (pointerSlots.ContainsKey(pointerId))
            {
                return;
            }
            finger = pointerSlots.ContainsValue(0) ? (pointerSlots.ContainsValue(1) ? -1 : 1) : 0;
            if (finger < 0)
            {
                return;
            }
            pointerSlots[pointerId] = finger;
            trackingIds[finger] = (byte)((trackingIds[finger] + 1) & 0x7F);
            SetFingerActive(finger, true);
            SetFingerId(finger, trackingIds[finger]);
        }
        else if (!pointerSlots.TryGetValue(pointerId, out finger))
        {
            return;
        }

        SetFingerPosition(finger, x, y);
        if (eventType is 2 or 4)
        {
            SetFingerActive(finger, false);
            pointerSlots.Remove(pointerId);
        }
        State.TouchpadPacketCounter++;
    }

    public void Dispose()
    {
        Controller.OutputReceived -= OnOutputReceived;
        if (Controller.UsbAudio is not null)
        {
            if (pcmHandler is not null)
            {
                Controller.UsbAudio.Output.FramesReceived -= pcmHandler;
            }
            if (pcmStreamingHandler is not null)
            {
                Controller.UsbAudio.Output.StreamingChanged -= pcmStreamingHandler;
            }
        }
        Controller.Dispose();
    }

    private void SetAxis(HMAxis axis, float value)
    {
        if (axis != HMAxis.None)
        {
            Axes[axis] = Math.Clamp(value, 0.0f, 1.0f);
        }
    }

    private float GetAxis(HMAxis axis, float fallback)
    {
        return axis != HMAxis.None && Axes.TryGetValue(axis, out float value) ? value : fallback;
    }

    private void SetFingerActive(int finger, bool active)
    {
        if (finger == 0)
        {
            State.TouchpadFinger0Active = active;
        }
        else
        {
            State.TouchpadFinger1Active = active;
        }
    }

    private void SetFingerId(int finger, byte id)
    {
        if (finger == 0)
        {
            State.TouchpadFinger0Id = id;
        }
        else
        {
            State.TouchpadFinger1Id = id;
        }
    }

    private void SetFingerPosition(int finger, float x, float y)
    {
        ushort nativeX = (ushort)Math.Round(Math.Clamp(x, 0.0f, 1.0f) * 1919.0f);
        ushort nativeY = (ushort)Math.Round(Math.Clamp(y, 0.0f, 1.0f) * 1079.0f);
        if (finger == 0)
        {
            State.TouchpadFinger0X = nativeX;
            State.TouchpadFinger0Y = nativeY;
        }
        else
        {
            State.TouchpadFinger1X = nativeX;
            State.TouchpadFinger1Y = nativeY;
        }
    }

    private void OnOutputReceived(HMController sender, HMOutputPacket packet)
    {
        outputHandler(this, packet);
    }
}

/// <summary>Runs the HIDMaestro SDK behind a versioned named-pipe boundary.</summary>
internal sealed class Host : IDisposable
{
    private const float EarthGravity = 9.80665f;
    private const int DsSensorScale = 8192;
    private const int DsGyroRange = 2000;
    private const int MaxPcmBytes = 3920;
    private const int PcmQueueCapacity = 32;
    private const int PipeInputBufferBytes = 64 * 1024;
    private const int PipeOutputBufferBytes = 512 * 1024;
    private const int InputReportIntervalMilliseconds = 8;
    private readonly NamedPipeServerStream pipe;
    private readonly object writeLock = new();
    private readonly Dictionary<uint, ControllerSlot> controllers = new();
    private readonly object controllersLock = new();
    private readonly ManualResetEvent inputReportStop = new(false);
    private readonly Thread inputReportThread;
    private readonly Stopwatch sensorClock = Stopwatch.StartNew();
    private readonly BlockingCollection<NativePcmFrame> pcmQueue = new(PcmQueueCapacity);
    private readonly Dictionary<uint, NativePcmDiagnosticWindow> pcmDiagnostics = new();
    private readonly Thread pcmWriterThread;
    private readonly object pcmIngressLock = new();
    private HMContext? context;
    private HMProfile? profile;
    private HMProfile? compositeProfile;
    private int droppedPcmFrames;

    internal Host(string pipeName)
    {
        pipe = new NamedPipeServerStream(
            pipeName,
            PipeDirection.InOut,
            1,
            PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous,
            PipeInputBufferBytes,
            PipeOutputBufferBytes);
        pcmWriterThread = new Thread(PcmWriterLoop)
        {
            IsBackground = true,
            Name = "HIDMaestro PCM writer",
        };
        pcmWriterThread.Start();
        inputReportThread = new Thread(InputReportLoop)
        {
            IsBackground = true,
            Name = "HIDMaestro DS5 input reports",
        };
        inputReportThread.Start();
    }

    /// <summary>Connect, initialize HIDMaestro, and process requests.</summary>
    internal int Run()
    {
        pipe.WaitForConnection();
        try
        {
            SendLog("Loading the embedded DualSense profile");
            context = new HMContext();
            context.LoadDefaultProfiles();
            profile = context.GetProfile("dualsense") ?? throw new InvalidOperationException("The embedded DualSense profile is unavailable.");
            compositeProfile = context.GetProfile("dualsense-composite");
            if (compositeProfile is null)
            {
                SendLog("The embedded DualSense composite profile is unavailable; native controller PCM is disabled");
            }
            if (!context.IsDriverInstalled)
            {
                SendLog("Installing the HIDMaestro driver");
                context.InstallDriver();
            }
            else
            {
                SendLog("The HIDMaestro driver is already installed");
            }
            SendStatus(Protocol.MessageType.Ready, 0, 0);
        }
        catch (Exception exception)
        {
            SendLog($"Initialization failed: {exception.Message}");
            SendStatus(Protocol.MessageType.Ready, 0, -1);
            return 1;
        }

        try
        {
            while (ReadMessage(out Protocol.MessageType type, out uint controllerId, out byte[] payload))
            {
                if (type == Protocol.MessageType.Shutdown)
                {
                    break;
                }
                Dispatch(type, controllerId, payload);
            }
        }
        catch (EndOfStreamException)
        {
            // Sunshine closed the connection.
        }
        catch (IOException)
        {
            // Sunshine closed the connection.
        }
        catch (Exception exception)
        {
            SendLog($"Protocol loop failed: {exception.Message}");
            return 2;
        }
        return 0;
    }

    public void Dispose()
    {
        inputReportStop.Set();
        if (Thread.CurrentThread != inputReportThread)
        {
            inputReportThread.Join();
        }
        inputReportStop.Dispose();
        lock (controllersLock)
        {
            foreach (ControllerSlot slot in controllers.Values)
            {
                slot.Dispose();
            }
            controllers.Clear();
        }
        pcmQueue.CompleteAdding();
        if (Thread.CurrentThread != pcmWriterThread)
        {
            pcmWriterThread.Join();
        }
        pcmQueue.Dispose();
        context?.Dispose();
        pipe.Dispose();
    }

    /// <summary>Validate protocol sizes without installing a driver.</summary>
    internal static bool SelfTest()
    {
        try
        {
            using HMContext testContext = new();
            testContext.LoadDefaultProfiles();
            HMProfile? testProfile = testContext.GetProfile("dualsense");
            HMProfile? testCompositeProfile = testContext.GetProfile("dualsense-composite");
            byte[] usbOutput = new byte[47];
            usbOutput[0] = 0x03;
            usbOutput[2] = 0x22;
            usbOutput[3] = 0x44;
            usbOutput[43] = 0x15;
            byte[] ridIncludedOutput = new byte[48];
            ridIncludedOutput[0] = 0x02;
            usbOutput.CopyTo(ridIncludedOutput, 1);
            bool parsedSeparatedReportId = TryGetDualSenseUsbOutput(0x02, usbOutput, out ReadOnlySpan<byte> separatedPayload) &&
                                             separatedPayload[2] == 0x22 && separatedPayload[3] == 0x44 && separatedPayload[43] == 0x15;
            bool parsedIncludedReportId = TryGetDualSenseUsbOutput(0x00, ridIncludedOutput, out ReadOnlySpan<byte> includedPayload) &&
                                             includedPayload[2] == 0x22 && includedPayload[3] == 0x44 && includedPayload[43] == 0x15;
            HMGamepadState inputState = new()
            {
                Buttons = HMButton.A | HMButton.X | HMButton.LeftBumper | HMButton.Back | HMButton.Guide | HMButton.Touchpad | HMButton.Misc1,
                Hat = HMHat.SouthWest,
                GyroPitch = -1234,
                GyroYaw = 2345,
                GyroRoll = -3456,
                AccelX = 4567,
                AccelY = -5678,
                AccelZ = 6789,
                SensorTimestamp = 0x12345678,
                TouchpadFinger0Active = true,
                TouchpadFinger0Id = 0x12,
                TouchpadFinger0X = 0x345,
                TouchpadFinger0Y = 0x234,
                TouchpadFinger1Active = false,
                TouchpadFinger1Id = 0x34,
                BatteryLevel = 7,
                BatteryCharging = true,
            };
            byte[] inputReport = new byte[DualSenseInputReport.DataLength];
            byte sequence = 0x5A;
            DualSenseInputReport.Encode(in inputState, 0.0f, 0.25f, 0.5f, 1.0f, 0.125f, 0.875f, ref sequence, inputReport);
            bool encodedInputReport = inputReport[0] == 0x00 && inputReport[1] == 0x40 &&
                                      inputReport[2] == 0x80 && inputReport[3] == 0xFF &&
                                      inputReport[4] == 0x20 && inputReport[5] == 0xDF &&
                                      inputReport[6] == 0x5A && sequence == 0x5B &&
                                      inputReport[7] == 0x35 && inputReport[8] == 0x1D && inputReport[9] == 0x07 &&
                                      BinaryPrimitives.ReadInt16LittleEndian(inputReport.AsSpan(15)) == -1234 &&
                                      BinaryPrimitives.ReadInt16LittleEndian(inputReport.AsSpan(17)) == 2345 &&
                                      BinaryPrimitives.ReadInt16LittleEndian(inputReport.AsSpan(19)) == -3456 &&
                                      BinaryPrimitives.ReadInt16LittleEndian(inputReport.AsSpan(21)) == 4567 &&
                                      BinaryPrimitives.ReadInt16LittleEndian(inputReport.AsSpan(23)) == -5678 &&
                                      BinaryPrimitives.ReadInt16LittleEndian(inputReport.AsSpan(25)) == 6789 &&
                                      BinaryPrimitives.ReadUInt32LittleEndian(inputReport.AsSpan(27)) == 0x12345678 &&
                                      inputReport.AsSpan(32, 4).SequenceEqual(new byte[] { 0x12, 0x45, 0x43, 0x23 }) &&
                                      inputReport.AsSpan(36, 4).SequenceEqual(new byte[] { 0xB4, 0x00, 0x00, 0x00 }) &&
                                       inputReport[52] == 0x17;
            byte[] testPcm = Enumerable.Range(0, 16).Select(value => (byte)value).ToArray();
            byte[] encodedPcm = BuildPcmPayload(new NativePcmFrame(2, 0x1234, 4, 16, 48000, testPcm));
            bool encodedPcmPayload = encodedPcm.Length == 24 &&
                                     BinaryPrimitives.ReadUInt16LittleEndian(encodedPcm) == 0x1234 &&
                                     encodedPcm[2] == 4 && encodedPcm[3] == 16 &&
                                     BinaryPrimitives.ReadUInt32LittleEndian(encodedPcm.AsSpan(4)) == 48000 &&
                                     encodedPcm.AsSpan(8).SequenceEqual(testPcm);
            byte[] diagnosticPcm = new byte[16];
            BinaryPrimitives.WriteInt16LittleEndian(diagnosticPcm, -1234);
            BinaryPrimitives.WriteInt16LittleEndian(diagnosticPcm.AsSpan(2), 2345);
            BinaryPrimitives.WriteInt16LittleEndian(diagnosticPcm.AsSpan(4), short.MinValue);
            BinaryPrimitives.WriteInt16LittleEndian(diagnosticPcm.AsSpan(6), 4567);
            BinaryPrimitives.WriteInt16LittleEndian(diagnosticPcm.AsSpan(8), -12);
            BinaryPrimitives.WriteInt16LittleEndian(diagnosticPcm.AsSpan(10), 34);
            BinaryPrimitives.WriteInt16LittleEndian(diagnosticPcm.AsSpan(12), -56);
            BinaryPrimitives.WriteInt16LittleEndian(diagnosticPcm.AsSpan(14), 78);
            int[] diagnosticPeaks = new int[4];
            NativePcmDiagnosticWindow.AccumulatePeaks(diagnosticPcm, 4, 16, diagnosticPeaks);
            bool measuredPcmPeaks = diagnosticPeaks.SequenceEqual(new[] { 1234, 2345, 32768, 4567 });
            using ManualResetEvent periodicStop = new(false);
            using CountdownEvent periodicTicks = new(2);
            Thread periodicThread = new(() => RunPeriodicLoop(periodicStop, 1, () =>
            {
                if (periodicTicks.CurrentCount > 0)
                {
                    periodicTicks.Signal();
                }
            }));
            periodicThread.Start();
            bool periodicReportsTicked = periodicTicks.Wait(TimeSpan.FromSeconds(1));
            periodicStop.Set();
            periodicThread.Join();

            return Protocol.HeaderSize == 16 &&
                   sizeof(uint) + (4 * sizeof(short)) + 4 == 16 &&
                   sizeof(uint) + (3 * sizeof(float)) + 4 == 20 &&
                   (3 * sizeof(float)) + 4 == 16 &&
                   4 + 10 + 10 == 24 &&
                   parsedSeparatedReportId &&
                   parsedIncludedReportId &&
                   encodedInputReport &&
                   encodedPcmPayload &&
                   measuredPcmPeaks &&
                   periodicReportsTicked &&
                   testProfile is not null &&
                   testCompositeProfile is not null &&
                   testProfile.Sticks.Count >= 2 &&
                   testProfile.Triggers.Count >= 2;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Install the driver and briefly create a virtual DualSense.</summary>
    internal static int DriverSelfTest()
    {
        try
        {
            using HMContext testContext = new();
            testContext.LoadDefaultProfiles();
            HMProfile testProfile = testContext.GetProfile("dualsense") ?? throw new InvalidOperationException("The embedded DualSense profile is unavailable.");
            testContext.InstallDriver();
            using HMController testController = testContext.CreateController(testProfile);
            string? lastOutput = null;
            testController.OutputReceived += (_, packet) =>
            {
                ReadOnlySpan<byte> data = packet.Data.Span;
                string summary = $"source={packet.Source} report=0x{packet.ReportId:X2} size={data.Length} data={Convert.ToHexString(data[..Math.Min(data.Length, 48)])}";
                if (!string.Equals(summary, lastOutput, StringComparison.Ordinal))
                {
                    Console.WriteLine($"Captured output: {summary}");
                    lastOutput = summary;
                }
            };
            HMGamepadState testState = new()
            {
                Axes = new Dictionary<HMAxis, float>(),
                Hat = HMHat.None,
                AccelY = DsSensorScale,
                BatteryLevel = 5,
            };
            foreach (var stick in testProfile.Sticks)
            {
                testState.Axes[stick.XAxis] = 0.5f;
                testState.Axes[stick.YAxis] = 0.5f;
            }
            foreach (var trigger in testProfile.Triggers)
            {
                testState.Axes[trigger.Axis] = 0.0f;
            }
            byte[] inputReport = new byte[DualSenseInputReport.DataLength];
            byte sequence = 0;
            DualSenseInputReport.Encode(in testState, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.0f, ref sequence, inputReport);
            testController.SubmitRawReport(inputReport);
            Console.WriteLine("HIDMaestro driver and virtual DualSense self-test passed.");
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"HIDMaestro driver self-test failed: {exception.Message}");
            return 1;
        }
    }

    /// <summary>Create a virtual DualSense and keep it connected for a diagnostic interval.</summary>
    /// <param name="holdSeconds">Number of seconds to keep the controller connected.</param>
    internal static int DriverHoldTest(int holdSeconds)
    {
        try
        {
            using HMContext testContext = new();
            testContext.LoadDefaultProfiles();
            HMProfile testProfile = testContext.GetProfile("dualsense") ?? throw new InvalidOperationException("The embedded DualSense profile is unavailable.");
            testContext.InstallDriver();
            using HMController testController = testContext.CreateController(testProfile);
            HMGamepadState testState = new()
            {
                Axes = new Dictionary<HMAxis, float>(),
                Hat = HMHat.None,
                AccelY = DsSensorScale,
                BatteryLevel = 5,
            };
            foreach (var stick in testProfile.Sticks)
            {
                testState.Axes[stick.XAxis] = 0.5f;
                testState.Axes[stick.YAxis] = 0.5f;
            }
            foreach (var trigger in testProfile.Triggers)
            {
                testState.Axes[trigger.Axis] = 0.0f;
            }
            Console.WriteLine($"HIDMaestro virtual DualSense will remain connected for {holdSeconds} seconds.");
            Console.WriteLine("A Cross-button pulse is sent once per second so browser HID/gamepad pages receive fresh input reports.");
            Console.WriteLine("Browser HID output reports will be printed below while their values change.");
            Stopwatch holdTimer = Stopwatch.StartNew();
            TimeSpan holdDuration = TimeSpan.FromSeconds(holdSeconds);
            byte[] inputReport = new byte[DualSenseInputReport.DataLength];
            byte sequence = 0;
            while (holdTimer.Elapsed < holdDuration)
            {
                testState.Buttons = holdTimer.ElapsedMilliseconds % 1000 < 150 ? HMButton.Cross : HMButton.None;
                DualSenseInputReport.Encode(in testState, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.0f, ref sequence, inputReport);
                testController.SubmitRawReport(inputReport);
                Thread.Sleep(TimeSpan.FromMilliseconds(8));
            }
            Console.WriteLine("HIDMaestro virtual DualSense diagnostic interval completed.");
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"HIDMaestro virtual DualSense hold test failed: {exception.Message}");
            return 1;
        }
    }

    private void Dispatch(Protocol.MessageType type, uint controllerId, byte[] payload)
    {
        lock (controllersLock)
        {
            switch (type)
            {
                case Protocol.MessageType.CreateController:
                    CreateController(controllerId, payload);
                    break;
                case Protocol.MessageType.DestroyController:
                    DestroyController(controllerId);
                    break;
                case Protocol.MessageType.GamepadState:
                    UpdateGamepad(controllerId, payload);
                    break;
                case Protocol.MessageType.Touch:
                    UpdateTouch(controllerId, payload);
                    break;
                case Protocol.MessageType.Motion:
                    UpdateMotion(controllerId, payload);
                    break;
                case Protocol.MessageType.Battery:
                    UpdateBattery(controllerId, payload);
                    break;
                default:
                    SendLog($"Ignoring unexpected request type {(ushort)type}");
                    break;
            }
        }
    }

    private void CreateController(uint id, ReadOnlySpan<byte> payload)
    {
        HMController? controller = null;
        ControllerSlot? slot = null;
        try
        {
            if (id >= 16 || controllers.ContainsKey(id))
            {
                throw new InvalidOperationException($"Controller index {id} is unavailable.");
            }
            bool useComposite = payload.Length switch
            {
                0 => false,
                4 => payload[0] != 0,
                _ => throw new InvalidOperationException($"Controller {id} create payload has invalid size {payload.Length}."),
            };
            HMProfile selectedProfile = useComposite
                ? compositeProfile ?? throw new InvalidOperationException("The DualSense composite profile is unavailable.")
                : profile!;
            SendLog($"Creating virtual DualSense controller {id} with profile {selectedProfile.Id}");
            try
            {
                controller = context!.CreateController(selectedProfile);
            }
            catch (Exception exception) when (useComposite)
            {
                SendLog($"Composite DualSense creation failed for controller {id}: {exception.Message}; falling back to the standard profile");
                selectedProfile = profile!;
                controller = context!.CreateController(selectedProfile);
            }
            SendLog($"HIDMaestro created the device for controller {id}");
            slot = new ControllerSlot(controller, selectedProfile, id, HandleOutput, HandleNativePcm, HandleNativePcmStreamingChanged);
            controller = null;
            controllers.Add(id, slot);
            slot.Submit(0);
            SendLog($"DS5 controller {id} is using full 63-byte USB input reports");
            if (slot.Controller.UsbAudio is not null)
            {
                HMAudioOutput output = slot.Controller.UsbAudio.Output;
                SendLog($"DS5 native PCM endpoint {id}: {output.Channels} channels, {output.SampleRateHz} Hz, {output.BitsPerSample}-bit, roles={string.Join(',', output.ChannelRoles)}");
            }
            SendStatus(Protocol.MessageType.ControllerCreated, id, 0);
        }
        catch (Exception exception)
        {
            if (controllers.Remove(id, out ControllerSlot? registeredSlot))
            {
                registeredSlot.Dispose();
            }
            else if (slot is not null)
            {
                slot.Dispose();
            }
            else
            {
                controller?.Dispose();
            }
            SendLog($"Controller {id} creation failed: {exception.Message}");
            SendStatus(Protocol.MessageType.ControllerCreated, id, -1);
        }
    }

    private void DestroyController(uint id)
    {
        if (controllers.Remove(id, out ControllerSlot? slot))
        {
            slot.Dispose();
        }
    }

    private void UpdateGamepad(uint id, ReadOnlySpan<byte> payload)
    {
        if (!TryGetController(id, payload, 16, out ControllerSlot? slot))
        {
            return;
        }
        slot.State.Buttons = (HMButton)BinaryPrimitives.ReadUInt32LittleEndian(payload);
        short lx = BinaryPrimitives.ReadInt16LittleEndian(payload[4..]);
        short ly = BinaryPrimitives.ReadInt16LittleEndian(payload[6..]);
        short rx = BinaryPrimitives.ReadInt16LittleEndian(payload[8..]);
        short ry = BinaryPrimitives.ReadInt16LittleEndian(payload[10..]);
        slot.State.Hat = (HMHat)payload[14];
        slot.SetAxes(
            NormalizeSigned(lx, false),
            NormalizeSigned(ly, true),
            NormalizeSigned(rx, false),
            NormalizeSigned(ry, true),
            payload[12] / 255.0f,
            payload[13] / 255.0f);
        StampAndSubmit(slot);
    }

    private void UpdateTouch(uint id, ReadOnlySpan<byte> payload)
    {
        if (!TryGetController(id, payload, 20, out ControllerSlot? slot))
        {
            return;
        }
        uint pointerId = BinaryPrimitives.ReadUInt32LittleEndian(payload);
        float x = BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(payload[4..]));
        float y = BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(payload[8..]));
        slot.ApplyTouch(pointerId, payload[16], x, y);
        StampAndSubmit(slot);
        if (!slot.LoggedTouchInput)
        {
            SendLog($"DS5 touch input {id}: event={payload[16]}, pointer={pointerId}, x={x:F4}, y={y:F4}");
            slot.LoggedTouchInput = true;
        }
    }

    private void UpdateMotion(uint id, ReadOnlySpan<byte> payload)
    {
        if (!TryGetController(id, payload, 16, out ControllerSlot? slot))
        {
            return;
        }
        float x = BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(payload));
        float y = BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(payload[4..]));
        float z = BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(payload[8..]));
        if (payload[12] == 1)
        {
            slot.State.AccelX = ScaleAndClamp(x / EarthGravity, DsSensorScale);
            slot.State.AccelY = ScaleAndClamp(y / EarthGravity, DsSensorScale);
            slot.State.AccelZ = ScaleAndClamp(z / EarthGravity, DsSensorScale);
            if (!slot.LoggedAccelerometerInput)
            {
                SendLog($"DS5 accelerometer input {id}: x={x:F4}, y={y:F4}, z={z:F4}");
                slot.LoggedAccelerometerInput = true;
            }
        }
        else if (payload[12] == 2)
        {
            slot.State.GyroPitch = ScaleAndClamp(x / DsGyroRange, short.MaxValue + 1.0f);
            slot.State.GyroYaw = ScaleAndClamp(y / DsGyroRange, short.MaxValue + 1.0f);
            slot.State.GyroRoll = ScaleAndClamp(z / DsGyroRange, short.MaxValue + 1.0f);
            if (!slot.LoggedGyroscopeInput)
            {
                SendLog($"DS5 gyroscope input {id}: x={x:F4}, y={y:F4}, z={z:F4}");
                slot.LoggedGyroscopeInput = true;
            }
        }
        else
        {
            return;
        }
        StampAndSubmit(slot);
    }

    private void UpdateBattery(uint id, ReadOnlySpan<byte> payload)
    {
        if (!TryGetController(id, payload, 4, out ControllerSlot? slot))
        {
            return;
        }
        byte state = payload[0];
        byte percentage = payload[1];
        slot.State.BatteryCharging = state == 3;
        slot.State.BatteryFull = state == 5;
        if (percentage <= 100)
        {
            slot.State.BatteryLevel = (byte)Math.Clamp((percentage + 5) / 10, 0, 10);
        }
        else if (state == 1)
        {
            slot.State.BatteryLevel = 0;
        }
        StampAndSubmit(slot);
    }

    private void HandleOutput(ControllerSlot slot, HMOutputPacket packet)
    {
        if (!TryGetDualSenseUsbOutput(packet.ReportId, packet.Data.Span, out ReadOnlySpan<byte> output))
        {
            if (!slot.LoggedUnsupportedOutput)
            {
                SendLog($"DS5 output {slot.Id} ignored: source={packet.Source}, report=0x{packet.ReportId:X2}, size={packet.Data.Length}");
                slot.LoggedUnsupportedOutput = true;
            }
            return;
        }

        byte valid0 = output[0];
        byte valid1 = output[1];

        if ((valid0 & 0x03) != 0)
        {
            byte rightMotor = output[2];
            byte leftMotor = output[3];
            Span<byte> rumble = stackalloc byte[4];
            BinaryPrimitives.WriteUInt16LittleEndian(rumble, (ushort)(leftMotor * 257));
            BinaryPrimitives.WriteUInt16LittleEndian(rumble[2..], (ushort)(rightMotor * 257));
            Send(Protocol.MessageType.Rumble, slot.Id, rumble);
            if (!slot.LoggedRumbleOutput && (leftMotor != 0 || rightMotor != 0))
            {
                SendLog($"DS5 native rumble {slot.Id}: flags=0x{valid0:X2}, heavy={leftMotor}, light={rightMotor}, source={packet.Source}");
                slot.LoggedRumbleOutput = true;
            }
        }

        byte triggerFlags = (byte)(valid0 & 0x0C);
        if (triggerFlags != 0)
        {
            ReadOnlySpan<byte> rightEffect = output.Slice(10, 11);
            ReadOnlySpan<byte> leftEffect = output.Slice(21, 11);
            Span<byte> triggers = stackalloc byte[24];
            triggers.Clear();
            triggers[0] = triggerFlags;
            triggers[1] = leftEffect[0];
            triggers[2] = rightEffect[0];
            leftEffect[1..].CopyTo(triggers[4..14]);
            rightEffect[1..].CopyTo(triggers[14..24]);
            Send(Protocol.MessageType.AdaptiveTriggers, slot.Id, triggers);
            if (!slot.LoggedAdaptiveTriggerOutput)
            {
                SendLog($"DS5 adaptive triggers {slot.Id}: flags=0x{triggerFlags:X2}, leftType=0x{leftEffect[0]:X2}, rightType=0x{rightEffect[0]:X2}");
                slot.LoggedAdaptiveTriggerOutput = true;
            }
        }

        if ((valid1 & 0x04) != 0)
        {
            Span<byte> lightbar = stackalloc byte[4];
            lightbar.Clear();
            output[44..47].CopyTo(lightbar);
            Send(Protocol.MessageType.Rgb, slot.Id, lightbar);
            if (!slot.LoggedRgbOutput)
            {
                SendLog($"DS5 lightbar {slot.Id}: rgb={output[44]},{output[45]},{output[46]}");
                slot.LoggedRgbOutput = true;
            }
        }

        if ((valid1 & 0x10) != 0)
        {
            Span<byte> playerIndicator = stackalloc byte[4];
            playerIndicator.Clear();
            playerIndicator[0] = output[43];
            Send(Protocol.MessageType.PlayerIndicator, slot.Id, playerIndicator);
            if (!slot.LoggedPlayerIndicatorOutput)
            {
                SendLog($"DS5 player indicator {slot.Id}: mask=0x{output[43]:X2}");
                slot.LoggedPlayerIndicatorOutput = true;
            }
        }
    }

    private void HandleNativePcmStreamingChanged(ControllerSlot slot, HMAudioOutput output, bool streaming)
    {
        SendLog($"DS5 native PCM stream {slot.Id} {(streaming ? "started" : "stopped")}: {output.Channels} channels at {output.SampleRateHz} Hz");
    }

    private void HandleNativePcm(ControllerSlot slot, HMAudioOutput output, ReadOnlyMemory<byte> pcm)
    {
        int sampleBytes = output.BitsPerSample / 8;
        int frameBytes = output.Channels * sampleBytes;
        if (output.Channels <= 0 || sampleBytes <= 0 || frameBytes <= 0 || pcm.IsEmpty || pcm.Length % frameBytes != 0)
        {
            return;
        }

        int maxChunkBytes = MaxPcmBytes - (MaxPcmBytes % frameBytes);
        lock (pcmIngressLock)
        {
            ReadOnlySpan<byte> remaining = pcm.Span;
            while (!remaining.IsEmpty)
            {
                int chunkBytes = Math.Min(remaining.Length, maxChunkBytes);
                chunkBytes -= chunkBytes % frameBytes;
                if (chunkBytes == 0)
                {
                    break;
                }
                NativePcmFrame frame = new(
                    slot.Id,
                    slot.NextPcmSequence(),
                    checked((byte)output.Channels),
                    checked((byte)output.BitsPerSample),
                    checked((uint)output.SampleRateHz),
                    remaining[..chunkBytes].ToArray());
                if (!pcmQueue.TryAdd(frame))
                {
                    if (pcmQueue.TryTake(out _))
                    {
                        Interlocked.Increment(ref droppedPcmFrames);
                    }
                    if (!pcmQueue.TryAdd(frame))
                    {
                        Interlocked.Increment(ref droppedPcmFrames);
                    }
                }
                remaining = remaining[chunkBytes..];
            }
        }
    }

    private void PcmWriterLoop()
    {
        foreach (NativePcmFrame frame in pcmQueue.GetConsumingEnumerable())
        {
            Send(Protocol.MessageType.ControllerPcm, frame.ControllerId, BuildPcmPayload(frame));
            int dropped = Interlocked.Exchange(ref droppedPcmFrames, 0);
            if (!pcmDiagnostics.TryGetValue(frame.ControllerId, out NativePcmDiagnosticWindow? diagnostics))
            {
                diagnostics = new NativePcmDiagnosticWindow();
                pcmDiagnostics.Add(frame.ControllerId, diagnostics);
            }
            diagnostics.Add(frame, dropped);
            if (diagnostics.ShouldReport())
            {
                SendLog($"DS5 native PCM stats {frame.ControllerId}: windows={diagnostics.Windows}, bytes={diagnostics.Bytes}, seq={frame.Sequence}, peak=[{diagnostics.Peaks[0]},{diagnostics.Peaks[1]},{diagnostics.Peaks[2]},{diagnostics.Peaks[3]}], dropped={diagnostics.Dropped}");
                diagnostics.Reset();
            }
        }
    }

    private static byte[] BuildPcmPayload(NativePcmFrame frame)
    {
        byte[] payload = new byte[8 + frame.Pcm.Length];
        BinaryPrimitives.WriteUInt16LittleEndian(payload, frame.Sequence);
        payload[2] = frame.Channels;
        payload[3] = frame.BitsPerSample;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4), frame.SampleRate);
        frame.Pcm.CopyTo(payload, 8);
        return payload;
    }

    /// <summary>Normalize a captured DualSense USB output report to its 47-byte report-ID-free payload.</summary>
    /// <param name="reportId">Report ID supplied separately by HIDMaestro.</param>
    /// <param name="data">Captured report data, normally without the report ID.</param>
    /// <param name="payload">Normalized 47-byte payload when successful.</param>
    /// <returns>True when the captured bytes contain a complete DualSense USB output report 0x02.</returns>
    private static bool TryGetDualSenseUsbOutput(byte reportId, ReadOnlySpan<byte> data, out ReadOnlySpan<byte> payload)
    {
        if (reportId == 0x02 && data.Length >= 47)
        {
            payload = data[0] == 0x02 && data.Length >= 48 ? data.Slice(1, 47) : data[..47];
            return true;
        }
        if (reportId == 0x00 && data.Length >= 48 && data[0] == 0x02)
        {
            payload = data.Slice(1, 47);
            return true;
        }
        payload = default;
        return false;
    }

    private bool ReadMessage(out Protocol.MessageType type, out uint controllerId, out byte[] payload)
    {
        byte[] header = new byte[Protocol.HeaderSize];
        if (!ReadExact(header))
        {
            type = default;
            controllerId = 0;
            payload = Array.Empty<byte>();
            return false;
        }
        uint magic = BinaryPrimitives.ReadUInt32LittleEndian(header);
        ushort version = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(4));
        type = (Protocol.MessageType)BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(6));
        uint payloadSize = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(8));
        controllerId = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(12));
        if (magic != Protocol.Magic || version != Protocol.Version || payloadSize > Protocol.MaxPayloadSize)
        {
            throw new InvalidDataException("Invalid Sunshine HIDMaestro protocol header.");
        }
        payload = new byte[payloadSize];
        return payload.Length == 0 || ReadExact(payload);
    }

    private bool ReadExact(Span<byte> destination)
    {
        int offset = 0;
        while (offset < destination.Length)
        {
            int count = pipe.Read(destination[offset..]);
            if (count == 0)
            {
                return false;
            }
            offset += count;
        }
        return true;
    }

    private void SendStatus(Protocol.MessageType type, uint id, int status)
    {
        Span<byte> payload = stackalloc byte[4];
        BinaryPrimitives.WriteInt32LittleEndian(payload, status);
        Send(type, id, payload);
    }

    private void SendLog(string message)
    {
        Send(Protocol.MessageType.Log, 0, Encoding.UTF8.GetBytes(message));
    }

    private void Send(Protocol.MessageType type, uint id, ReadOnlySpan<byte> payload)
    {
        lock (writeLock)
        {
            try
            {
                Span<byte> header = stackalloc byte[Protocol.HeaderSize];
                BinaryPrimitives.WriteUInt32LittleEndian(header, Protocol.Magic);
                BinaryPrimitives.WriteUInt16LittleEndian(header[4..], Protocol.Version);
                BinaryPrimitives.WriteUInt16LittleEndian(header[6..], (ushort)type);
                BinaryPrimitives.WriteUInt32LittleEndian(header[8..], (uint)payload.Length);
                BinaryPrimitives.WriteUInt32LittleEndian(header[12..], id);
                pipe.Write(header);
                if (!payload.IsEmpty)
                {
                    pipe.Write(payload);
                }
                // PipeStream writes are handed directly to the kernel. FlushFileBuffers would
                // synchronously wait for Sunshine to consume every PCM window and turns normal
                // control-thread scheduling jitter into avoidable audio drops.
            }
            catch (IOException)
            {
                // Sunshine disconnected while HIDMaestro was raising output.
            }
            catch (ObjectDisposedException)
            {
                // The host is already shutting down.
            }
        }
    }

    private bool TryGetController(uint id, ReadOnlySpan<byte> payload, int expectedSize, [NotNullWhen(true)] out ControllerSlot? slot)
    {
        slot = null;
        if (payload.Length != expectedSize || !controllers.TryGetValue(id, out slot))
        {
            return false;
        }
        return true;
    }

    private void StampAndSubmit(ControllerSlot slot)
    {
        uint timestamp = (uint)(sensorClock.Elapsed.TotalMicroseconds % (uint.MaxValue + 1.0));
        slot.Submit(timestamp);
    }

    /// <summary>Continuously submit complete DS5 reports even when client input is unchanged.</summary>
    private void InputReportLoop()
    {
        RunPeriodicLoop(inputReportStop, InputReportIntervalMilliseconds, () =>
        {
            lock (controllersLock)
            {
                foreach (ControllerSlot slot in controllers.Values)
                {
                    try
                    {
                        StampAndSubmit(slot);
                    }
                    catch (Exception exception)
                    {
                        if (!slot.LoggedInputReportFailure)
                        {
                            SendLog($"DS5 periodic input report {slot.Id} failed: {exception.Message}");
                            slot.LoggedInputReportFailure = true;
                        }
                    }
                }
            }
        });
    }

    /// <summary>Invoke an action at a fixed interval until a stop event is signaled.</summary>
    /// <param name="stopEvent">Event that terminates the loop.</param>
    /// <param name="intervalMilliseconds">Delay between actions in milliseconds.</param>
    /// <param name="action">Action invoked after each completed interval.</param>
    private static void RunPeriodicLoop(WaitHandle stopEvent, int intervalMilliseconds, Action action)
    {
        while (!stopEvent.WaitOne(intervalMilliseconds))
        {
            action();
        }
    }

    private static float NormalizeSigned(short value, bool invert)
    {
        float normalized = (value - (float)short.MinValue) / ushort.MaxValue;
        return invert ? 1.0f - normalized : normalized;
    }

    private static short ScaleAndClamp(float value, float scale)
    {
        return (short)Math.Clamp((int)Math.Round(value * scale), short.MinValue, short.MaxValue);
    }

}

/// <summary>Command-line entry point.</summary>
internal static class Program
{
    private static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--self-test")
        {
            bool passed = Host.SelfTest();
            Console.WriteLine(passed ? "HIDMaestro host self-test passed." : "HIDMaestro host self-test failed.");
            return passed ? 0 : 1;
        }
        if (args.Length == 1 && args[0] == "--driver-self-test")
        {
            return Host.DriverSelfTest();
        }
        if (args.Length == 2 && args[0] == "--driver-hold-test" &&
            int.TryParse(args[1], out int holdSeconds) && holdSeconds is >= 1 and <= 86400)
        {
            return Host.DriverHoldTest(holdSeconds);
        }
        if (args.Length != 2 || args[0] != "--pipe" || string.IsNullOrWhiteSpace(args[1]))
        {
            Console.Error.WriteLine("Usage: sunshine-hidmaestro-host --pipe <name> | --self-test | --driver-self-test | --driver-hold-test <seconds>");
            return 64;
        }

        using Host host = new(args[1]);
        return host.Run();
    }
}
