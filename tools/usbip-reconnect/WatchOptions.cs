using System.Net;
using System.Net.Sockets;

namespace Sunshine.UsbIpReconnect;

/// <summary>Fixed Android export endpoint and parent-session identity; arbitrary targets are not accepted.</summary>
internal sealed record WatchOptions(IPAddress Host, int OwnerPid, uint SessionId)
{
    /// <summary>Fixed port of the Android export.</summary>
    public const int Port = 3240;
    /// <summary>Single-device bus identity advertised by Android.</summary>
    public const string BusId = "1-1";
    /// <summary>Stop event created by the parent for this launch session.</summary>
    public string StopEvent => $"Local\\StarCoreUsbIpStop-{OwnerPid}-{SessionId}";

    /// <summary>Accept only canonical RFC1918 IPv4 peers explicitly supplied by a streaming session.</summary>
    public static bool TryParse(string[] args, out WatchOptions? options)
    {
        options = null;
        if (args.Length != 4 || args[0] != "--watch" || !IPAddress.TryParse(args[1], out var host)
                || host.AddressFamily != AddressFamily.InterNetwork || host.ToString() != args[1]
                || !int.TryParse(args[2], out int pid) || pid <= 0 || !uint.TryParse(args[3], out uint session)) return false;
        byte[] bytes = host.GetAddressBytes();
        bool local = bytes[0] == 10 || (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31)
                || (bytes[0] == 192 && bytes[1] == 168);
        if (!local) return false;
        options = new WatchOptions(host, pid, session);
        return true;
    }
}
