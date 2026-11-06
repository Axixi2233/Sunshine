using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Reflection;

namespace Sunshine.UsbIpReconnect;

/// <summary>Reconnects one Android USB/IP export while its Sunshine process is alive.</summary>
internal static class Program
{
    /// <summary>Prepare the bundled transport or watch the explicitly opted-in stream client's export.</summary>
    private static async Task<int> Main(string[] args)
    {
        try {
            if (args.SequenceEqual(new[] { "--prepare-driver" })) {
                SdkTransport.Prepare(Log);
                Log("USB/IP transport is ready");
                return 0;
            }
            if (args.SequenceEqual(new[] { "--check-sdk" })) {
                _ = new SdkTransport(new WatchOptions(IPAddress.Parse("192.168.1.1"), 1, 0));
                Log("Embedded USB/IP SDK interfaces are available");
                return 0;
            }
            if (!WatchOptions.TryParse(args, out var options) || options is null) {
                Console.Error.WriteLine("Usage: usbip-reconnect --prepare-driver | --check-sdk | --watch <private IPv4> <Sunshine PID> <session ID>");
                return 2;
            }
            using var owner = Process.GetProcessById(options.OwnerPid);
            if (!owner.ProcessName.Equals("sunshine", StringComparison.OrdinalIgnoreCase)) return 2;
            using var stopEvent = EventWaitHandle.OpenExisting(options.StopEvent);
            using var singleton = new EventWaitHandle(false, EventResetMode.ManualReset,
                    $"Local\\StarCoreUsbIp-{options.Host}-{WatchOptions.Port}-{WatchOptions.BusId}", out bool createdNew);
            if (!createdNew) { Log("This USB/IP export is already watched"); return 2; }
            Log($"Watching {options.Host}:{WatchOptions.Port}/{WatchOptions.BusId} for Sunshine PID {options.OwnerPid}, session {options.SessionId}");
            SdkTransport.Prepare(Log);
            if (owner.HasExited || stopEvent.WaitOne(0)) {
                Log("Stream ended while its USB/IP driver was being prepared");
                return 0;
            }
            using var cancellation = new CancellationTokenSource();
            var watcher = new ReconnectWatcher(new SdkTransport(options),
                    _ => IsExportReady(options.Host, WatchOptions.Port, WatchOptions.BusId), Log, TimeSpan.FromMilliseconds(500));
            var task = Task.Run(() => watcher.Run(cancellation.Token));
            while (!task.IsCompleted && !owner.HasExited && !stopEvent.WaitOne(0)) await Task.Delay(100);
            cancellation.Cancel();
            watcher.CancelAttach();
            await task.WaitAsync(TimeSpan.FromSeconds(3));
            Log("Stream ended; USB/IP watcher stopped");
            return 0;
        } catch (Exception error) {
            Log("USB/IP helper failed: " + (error.InnerException ?? error).Message);
            return 1;
        }
    }

    /// <summary>Probe DEVLIST with a bounded timeout before issuing a potentially blocking driver import.</summary>
    private static async Task<bool> IsExportReady(IPAddress host, int port, string busId)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromMilliseconds(700));
        using var socket = new TcpClient { NoDelay = true };
        try
        {
            await socket.ConnectAsync(host, port, timeout.Token);
            using var stream = socket.GetStream();
            await stream.WriteAsync(new byte[] { 1, 0x11, 0x80, 5, 0, 0, 0, 0 }, timeout.Token);
            byte[] header = new byte[12];
            await stream.ReadExactlyAsync(header, timeout.Token);
            // The Android exporter advertises exactly one device. Reject errors and incompatible peers.
            if (!header.AsSpan().SequenceEqual(new byte[] { 1, 0x11, 0, 5, 0, 0, 0, 0, 0, 0, 0, 1 }))
            {
                return false;
            }
            byte[] descriptor = new byte[312];
            await stream.ReadExactlyAsync(descriptor, timeout.Token);
            byte[] expectedId = new byte[32];
            System.Text.Encoding.ASCII.GetBytes(busId).CopyTo(expectedId, 0);
            // Android authorizes supported input devices. The Windows watcher follows the bus ID
            // across replacements, including devices with a different vendor or product ID.
            return descriptor.AsSpan(256, 32).SequenceEqual(expectedId);
        }
        catch (Exception error) when (error is IOException or SocketException or OperationCanceledException)
        {
            return false;
        }
    }

    /// <summary>Record reconnect transitions for correlating the Windows and Android timelines.</summary>
    private static void Log(string text)
    {
        string line = $"{DateTimeOffset.Now:yyyy-MM-dd HH:mm:ss.fff zzz} [{Environment.ProcessId}] {text}";
        Console.WriteLine(line);
        try {
            string directory = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "config"));
            Directory.CreateDirectory(directory);
            File.AppendAllText(Path.Combine(directory, "usbip.log"), line + Environment.NewLine);
        } catch (Exception error) when (error is IOException or UnauthorizedAccessException) {
            // Diagnostics must not prevent importing or releasing the device.
        }
    }
}
