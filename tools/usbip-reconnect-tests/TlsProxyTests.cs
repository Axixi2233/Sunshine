using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using Sunshine.UsbIpReconnect;

/// <summary>Exercise real IPv6 TLS, exact paired-certificate pinning and per-session token rejection.</summary>
internal static class TlsProxyTests
{
    public static async Task Run()
    {
        using var key = RSA.Create(2048);
        var request = new CertificateRequest("CN=NVIDIA GameStream Client", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        using var generated = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddMinutes(-5), DateTimeOffset.UtcNow.AddDays(1));
        // Schannel's server test fixture requires an imported key, unlike Android's TLS provider.
        using var certificate = X509CertificateLoader.LoadPkcs12(generated.Export(X509ContentType.Pfx), null, X509KeyStorageFlags.DefaultKeySet);
        const string token = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
        string bootstrap = JsonSerializer.Serialize(new { token, certificate = certificate.ExportCertificatePem() });
        var credentials = await TlsCredentials.Read(new StringReader(bootstrap));
        if (!credentials.Matches(certificate) || credentials.Matches(null)) throw new Exception("Incorrect certificate pin");
        foreach (string invalid in new[] { "", new string('x', 4097), JsonSerializer.Serialize(new { token = "bad", certificate = certificate.ExportCertificatePem() }) }) {
            bool rejected = false;
            try { await TlsCredentials.Read(new StringReader(invalid)); } catch (Exception error) when (error is IOException or JsonException) { rejected = true; }
            if (!rejected) throw new Exception("Invalid TLS bootstrap accepted");
        }
        foreach (string scenario in new[] { "valid", "wrong-pin", "wrong-token" }) {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(12));
            using var listener = new TcpListener(IPAddress.IPv6Loopback, 0);
            listener.Start();
            int port = ((IPEndPoint)listener.LocalEndpoint).Port;
            var server = Task.Run(async () => {
                try {
                    using var tcp = await listener.AcceptTcpClientAsync(timeout.Token);
                    using var tls = new SslStream(tcp.GetStream());
                    await tls.AuthenticateAsServerAsync(new SslServerAuthenticationOptions {
                        ServerCertificate = certificate, EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13,
                    }, timeout.Token);
                    byte[] receivedToken = new byte[64];
                    await tls.ReadExactlyAsync(receivedToken, timeout.Token);
                    if (!CryptographicOperations.FixedTimeEquals(receivedToken, Encoding.ASCII.GetBytes(token))) return;
                    byte[] header = new byte[8];
                    await tls.ReadExactlyAsync(header, timeout.Token);
                    if (!header.SequenceEqual(new byte[] { 1, 17, 128, 5, 0, 0, 0, 0 })) throw new Exception("Tunnel corrupted USB/IP request");
                    await tls.WriteAsync(new byte[] { 1, 17, 0, 5, 0, 0, 0, 0, 0, 0, 0, 1 }, timeout.Token);
                    byte[] descriptor = new byte[312];
                    Encoding.ASCII.GetBytes("1-1").CopyTo(descriptor, 256);
                    await tls.WriteAsync(descriptor.AsMemory(0, 120), timeout.Token);
                    await Task.Delay(10, timeout.Token);
                    await tls.WriteAsync(descriptor.AsMemory(120), timeout.Token);
                } catch (Exception error) when (scenario == "wrong-pin" && error is AuthenticationException or IOException) {}
            });
            var actual = scenario == "wrong-pin" ? credentials with { CertificateHash = new byte[32] }
                : scenario == "wrong-token" ? credentials with { Token = new string('A', 64) } : credentials;
            await using (var proxy = new TlsUsbIpProxy(IPAddress.IPv6Loopback, port, actual, Console.WriteLine)) {
                bool ready = await Sunshine.UsbIpReconnect.Program.IsExportReady(IPAddress.Loopback, proxy.LocalPort, "1-1", 6000);
                await server;
                if (ready != (scenario == "valid")) throw new Exception("TLS tunnel validation failed: " + scenario);
            }
            Console.WriteLine("PASS TLS-IPv6-" + scenario);
        }
        foreach (string host in new[] { "2001:db8:abcd::123", "2408:1234::1" }) {
            if (!WatchOptions.TryParse(new[] { "--watch-tls", host, "100", "1" }, out var options, Array.Empty<IPNetwork>()) || !options!.Tls)
                throw new Exception("Public IPv6 TLS peer rejected");
            if (WatchOptions.TryParse(new[] { "--watch", host, "100", "1" }, out _, Array.Empty<IPNetwork>()))
                throw new Exception("Unencrypted public IPv6 peer accepted");
        }
        foreach (string host in new[] { "8.8.8.8", "::1", "ff02::1", "::", "::ffff:8.8.8.8" })
            if (WatchOptions.TryParse(new[] { "--watch-tls", host, "100", "1" }, out _)) throw new Exception("Unsafe TLS target accepted");
        Console.WriteLine("PASS TLS-public-peer-policy");

        using var stopTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        using var pendingListener = new TcpListener(IPAddress.IPv6Loopback, 0);
        pendingListener.Start();
        var authenticated = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var pendingServer = Task.Run(async () => {
            using var tcp = await pendingListener.AcceptTcpClientAsync(stopTimeout.Token);
            using var tls = new SslStream(tcp.GetStream());
            await tls.AuthenticateAsServerAsync(new SslServerAuthenticationOptions { ServerCertificate = certificate }, stopTimeout.Token);
            await tls.ReadExactlyAsync(new byte[64], stopTimeout.Token);
            authenticated.SetResult();
            try {
                int read = await tls.ReadAsync(new byte[1], stopTimeout.Token);
                if (read != 0) throw new Exception("Unexpected data after tunnel cancellation");
            } catch (IOException) {}
        });
        var pendingProxy = new TlsUsbIpProxy(IPAddress.IPv6Loopback, ((IPEndPoint)pendingListener.LocalEndpoint).Port, credentials, Console.WriteLine);
        using var local = new TcpClient();
        await local.ConnectAsync(IPAddress.Loopback, pendingProxy.LocalPort, stopTimeout.Token);
        await authenticated.Task.WaitAsync(stopTimeout.Token);
        await pendingProxy.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(3));
        await pendingServer;
        Console.WriteLine("PASS TLS-cancels-idle-tunnel");
    }
}
