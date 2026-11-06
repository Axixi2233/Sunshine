using System.Net;
using System.Net.Sockets;
using System.Reflection;

await LifecycleTests.Run();

// Exercise the real DEVLIST probe against bounded loopback fixtures without touching the USB driver.
var assembly = Assembly.Load("usbip-reconnect");
var method = assembly.GetType("Sunshine.UsbIpReconnect.Program")!
    .GetMethod("IsExportReady", BindingFlags.NonPublic | BindingFlags.Static)!;
foreach (string scenario in new[] { "valid-fragmented", "wrong-busid", "other-input-device", "bad-header", "eof" })
{
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
    using var listener = new TcpListener(IPAddress.Loopback, 0);
    listener.Start();
    int port = ((IPEndPoint)listener.LocalEndpoint).Port;
    var server = Task.Run(async () => {
        using var socket = await listener.AcceptTcpClientAsync(timeout.Token);
        using var stream = socket.GetStream();
        var request = new byte[8];
        await stream.ReadExactlyAsync(request, timeout.Token);
        if (!request.SequenceEqual(new byte[] { 1, 0x11, 0x80, 5, 0, 0, 0, 0 }))
            throw new Exception("Incorrect DEVLIST request");
        if (scenario == "eof") return;
        byte[] header = { 1, 0x11, 0, 5, 0, 0, 0, 0, 0, 0, 0, 1 };
        if (scenario == "bad-header") header[7] = 1;
        await stream.WriteAsync(header);
        if (scenario == "bad-header") return;
        var descriptor = new byte[312];
        System.Text.Encoding.ASCII.GetBytes(scenario == "wrong-busid" ? "1-2" : "1-1").CopyTo(descriptor, 256);
        descriptor[300] = scenario == "other-input-device" ? (byte)0x12 : (byte)0x05;
        descriptor[301] = 0x4c;
        descriptor[302] = 0x0c;
        descriptor[303] = 0xe6;
        await stream.WriteAsync(descriptor.AsMemory(0, 121));
        await Task.Delay(10);
        await stream.WriteAsync(descriptor.AsMemory(121));
    });
    bool result = await (Task<bool>)method.Invoke(null, new object[] { IPAddress.Loopback, port, "1-1" })!;
    await server;
    if (result != (scenario == "valid-fragmented" || scenario == "other-input-device"))
        throw new Exception($"Incorrect result for {scenario}");
    Console.WriteLine($"PASS {scenario}");
}
