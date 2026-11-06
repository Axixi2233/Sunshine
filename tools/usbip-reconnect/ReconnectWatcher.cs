using System.Net.Sockets;
using System.Reflection;

namespace Sunshine.UsbIpReconnect;

/// <summary>SDK boundary that can be exercised without installing or attaching a USB driver.</summary>
internal interface IUsbIpTransport
{
    /// <summary>Find the exact remote export's current VHCI port.</summary>
    int Find();
    /// <summary>Import the fixed remote export.</summary>
    int Attach();
    /// <summary>Cancel retries for this exact export.</summary>
    void StopAttachAttempts();
    /// <summary>Detach this export after watcher ownership has been verified.</summary>
    void Detach();
}

/// <summary>Reconnect one export and clean up only imports or pending attempts this watcher created.</summary>
internal sealed class ReconnectWatcher(IUsbIpTransport transport, Func<CancellationToken, Task<bool>> ready,
        Action<string> log, TimeSpan interval)
{
    private int attempted;

    /// <summary>Interrupt the exact pending location when the parent session closes during Attach.</summary>
    public void CancelAttach()
    {
        if (Volatile.Read(ref attempted) == 0) return;
        try { transport.StopAttachAttempts(); }
        catch (Exception error) { log("USB/IP cancellation: " + (error.InnerException ?? error).Message); }
    }

    /// <summary>Poll an existing import or issue a bounded readiness probe before each attach attempt.</summary>
    public async Task Run(CancellationToken cancellation)
    {
        int ownedPort = -1;
        int lastPort = -1;
        try {
            while (!cancellation.IsCancellationRequested) {
                try {
                    int port = transport.Find();
                    if (port > 0 && ownedPort > 0 && port != ownedPort) {
                        Volatile.Write(ref attempted, 0);
                        ownedPort = -1;
                    }
                    if (port != lastPort) {
                        log(port > 0 ? $"Imported on VHCI port {port}" : "Waiting for Android export");
                        lastPort = port;
                    }
                    if (port <= 0 && await ready(cancellation) && !cancellation.IsCancellationRequested) {
                        CancelAttach();
                        Volatile.Write(ref attempted, 1);
                        ownedPort = transport.Attach();
                        log($"Attached on VHCI port {ownedPort}");
                    }
                } catch (Exception error) when (error is TargetInvocationException or IOException or SocketException) {
                    log("Reconnect pending: " + (error.InnerException ?? error).Message);
                }
                await Task.Delay(interval, cancellation);
            }
        } catch (OperationCanceledException) when (cancellation.IsCancellationRequested) {
            // The owning stream or parent process ended.
        } finally {
            if (Volatile.Read(ref attempted) != 0) {
                CancelAttach();
                try {
                    if (ownedPort > 0 && transport.Find() == ownedPort) transport.Detach();
                } catch (Exception error) { log("USB/IP cleanup: " + (error.InnerException ?? error).Message); }
            }
        }
    }
}
