using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Reflection;

namespace Sunshine.UsbIpReconnect;

/// <summary>Reconnects one Android USB/IP export while its Sunshine process is alive.</summary>
internal static class Program
{
    /// <summary>Watch an exact host, TCP port and bus ID using the installed HIDMaestro SDK.</summary>
    private static async Task<int> Main(string[] args)
    {
        if (args.Length != 5 || !IPAddress.TryParse(args[1], out var host)
                || host.AddressFamily != AddressFamily.InterNetwork
                || !int.TryParse(args[2], out var port) || port < 1 || port > 65535
                || args[3].Length == 0 || args[3].Length > 31
                || !int.TryParse(args[4], out var ownerPid))
        {
            Console.Error.WriteLine("Usage: usbip-reconnect <HIDMaestro.Core.dll> <IPv4> <port> <busid> <Sunshine PID>");
            return 2;
        }

        using var owner = Process.GetProcessById(ownerPid);
        if (!owner.ProcessName.Equals("sunshine", StringComparison.OrdinalIgnoreCase))
        {
            Console.Error.WriteLine("The owner process must be Sunshine.");
            return 2;
        }
        // A named event's lifetime follows its open handles without thread-affine ownership.
        using var singleton = new EventWaitHandle(false, EventResetMode.ManualReset,
            $"Local\\StarCoreUsbIp-{host}-{port}-{args[3]}", out bool createdNew);
        if (!createdNew)
        {
            Console.Error.WriteLine("This USB/IP location is already being watched.");
            return 2;
        }

        var sdk = Assembly.LoadFrom(Path.GetFullPath(args[0]));
        var client = sdk.GetType("HIDMaestro.Internal.Usbip.VhciClient", throwOnError: true)!;
        var locationTypes = new[] { typeof(string), typeof(int), typeof(string) };
        var attach = Method(client, "Attach", locationTypes);
        var stop = Method(client, "StopAttachAttempts", locationTypes);
        var find = Method(client, "FindLocation", locationTypes);
        object[] location = { host.ToString(), port, args[3] };
        Log($"Watching {host}:{port}/{args[3]} for Sunshine PID {ownerPid}");
        int lastPort = -1;
        while (!owner.HasExited)
        {
            try
            {
                int currentPort = (int)find.Invoke(null, location)!;
                if (currentPort != lastPort)
                {
                    Log(currentPort > 0 ? $"Imported on VHCI port {currentPort}" : "Waiting for Android export");
                    lastPort = currentPort;
                }
                if (currentPort <= 0)
                {
                    // Cancel only this location's slow kernel retry, including an old pending timer.
                    stop.Invoke(null, location);
                    if (await IsExportReady(host, port, args[3]))
                    {
                        var elapsed = Stopwatch.StartNew();
                        int importedPort = (int)attach.Invoke(null, location)!;
                        Log($"Attached on VHCI port {importedPort} in {elapsed.ElapsedMilliseconds} ms");
                    }
                }
            }
            catch (Exception error) when (error is TargetInvocationException or IOException or SocketException)
            {
                Log($"Reconnect pending: {(error.InnerException ?? error).Message}");
                await Task.Delay(1000);
            }
            await Task.Delay(500);
        }
        Log("Sunshine exited; reconnect watcher stopped");
        return 0;
    }

    /// <summary>Resolve an exact SDK signature so an incompatible SDK fails before starting the watcher.</summary>
    private static MethodInfo Method(Type client, string name, Type[] arguments) =>
        client.GetMethod(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static, arguments)
            ?? throw new MissingMethodException(client.FullName, name);

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
    private static void Log(string text) =>
        Console.WriteLine($"{DateTimeOffset.Now:yyyy-MM-dd HH:mm:ss.fff zzz} {text}");
}
