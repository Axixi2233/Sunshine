using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;

namespace Sunshine.UsbIpReconnect;

/// <summary>Session secret and the exact paired Android certificate, received only over an inherited pipe.</summary>
internal sealed record TlsCredentials(string Token, byte[] CertificateHash)
{
    /// <summary>Read a bounded bootstrap. Secrets never appear in command-line arguments or logs.</summary>
    public static async Task<TlsCredentials> Read(TextReader reader)
    {
        char[] buffer = new char[4097];
        int length = await reader.ReadBlockAsync(buffer);
        if (length == 0 || length > 4096) throw new IOException("Invalid USB/IP TLS bootstrap");
        using var json = JsonDocument.Parse(new string(buffer, 0, length));
        string token = json.RootElement.GetProperty("token").GetString() ?? "";
        if (token.Length != 64 || !token.All(Uri.IsHexDigit)) throw new IOException("Invalid USB/IP session credential");
        using var certificate = X509Certificate2.CreateFromPem(json.RootElement.GetProperty("certificate").GetString() ?? "");
        return new TlsCredentials(token, certificate.GetCertHash(HashAlgorithmName.SHA256));
    }

    /// <summary>Accept only the certificate already authenticated by Moonlight pairing, including self-signed peers.</summary>
    public bool Matches(X509Certificate? certificate) => certificate is not null
        && CryptographicOperations.FixedTimeEquals(CertificateHash, certificate.GetCertHash(HashAlgorithmName.SHA256));
}

/// <summary>Bridge the local-only VHCI connection into a certificate-pinned, session-authenticated TLS export.</summary>
internal sealed class TlsUsbIpProxy : IAsyncDisposable
{
    private readonly TcpListener listener = new(IPAddress.Loopback, 0);
    private readonly CancellationTokenSource stopping = new();
    private readonly List<Task> connections = new();
    private readonly IPAddress host;
    private readonly int port;
    private readonly TlsCredentials credentials;
    private readonly Action<string> log;
    private readonly Task accepting;
    private string? lastError;

    /// <summary>Local ephemeral endpoint used only by the embedded Windows driver.</summary>
    public int LocalPort => ((IPEndPoint)listener.LocalEndpoint).Port;

    /// <summary>Listen on IPv4 loopback only; the remote Android endpoint follows the stream's family.</summary>
    public TlsUsbIpProxy(IPAddress host, int port, TlsCredentials credentials, Action<string> log)
    {
        this.host = host;
        this.port = port;
        this.credentials = credentials;
        this.log = log;
        listener.Start(8);
        accepting = Accept();
    }

    /// <summary>Accept independent readiness probes, imports and driver reconnects until the stream closes.</summary>
    private async Task Accept()
    {
        try {
            while (!stopping.IsCancellationRequested) {
                var client = await listener.AcceptTcpClientAsync(stopping.Token);
                connections.RemoveAll(task => task.IsCompleted);
                connections.Add(Relay(client));
            }
        } catch (OperationCanceledException) when (stopping.IsCancellationRequested) {}
        catch (SocketException) when (stopping.IsCancellationRequested) {}
        catch (ObjectDisposedException) when (stopping.IsCancellationRequested) {}
    }

    /// <summary>Pin Android's paired identity, authenticate this session, then relay USB/IP bytes unchanged.</summary>
    private async Task Relay(TcpClient local)
    {
        using (local)
        using (var remote = new TcpClient(host.AddressFamily) { NoDelay = true })
        using (var lifetime = CancellationTokenSource.CreateLinkedTokenSource(stopping.Token)) {
            local.NoDelay = true;
            try {
                using var handshake = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
                handshake.CancelAfter(TimeSpan.FromSeconds(4));
                await remote.ConnectAsync(host, port, handshake.Token);
                using var tls = new SslStream(remote.GetStream(), false, (_, certificate, _, _) => credentials.Matches(certificate));
                await tls.AuthenticateAsClientAsync(new SslClientAuthenticationOptions {
                    TargetHost = "starcore-usbip",
                    EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13,
                    CertificateRevocationCheckMode = X509RevocationMode.NoCheck,
                }, handshake.Token);
                await tls.WriteAsync(Encoding.ASCII.GetBytes(credentials.Token), handshake.Token);
                handshake.CancelAfter(Timeout.InfiniteTimeSpan);
                lastError = null;
                using var plain = local.GetStream();
                Task send = plain.CopyToAsync(tls, 65536, lifetime.Token);
                Task receive = tls.CopyToAsync(plain, 65536, lifetime.Token);
                await Task.WhenAny(send, receive);
                lifetime.Cancel();
                try { await Task.WhenAll(send, receive); } catch (OperationCanceledException) when (lifetime.IsCancellationRequested) {}
            } catch (Exception error) when (error is IOException or SocketException or AuthenticationException or OperationCanceledException) {
                if (!stopping.IsCancellationRequested && error.Message != lastError) {
                    lastError = error.Message;
                    log("USB/IP TLS connection pending: " + error.Message);
                }
            }
        }
    }

    /// <summary>Stop all sockets after the watcher has released its own VHCI import.</summary>
    public async ValueTask DisposeAsync()
    {
        stopping.Cancel();
        listener.Stop();
        await accepting;
        await Task.WhenAll(connections);
        stopping.Dispose();
    }
}
