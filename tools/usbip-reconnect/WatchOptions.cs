using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;

namespace Sunshine.UsbIpReconnect;

/// <summary>Fixed Android export endpoint and parent-session identity; arbitrary targets are not accepted.</summary>
internal sealed record WatchOptions(IPAddress Host, int OwnerPid, uint SessionId)
{
    /// <summary>Public IPv6 is permitted only after the paired host negotiated encrypted transport.</summary>
    public bool Tls { get; init; }
    /// <summary>Fixed port of the Android export.</summary>
    public const int Port = 3240;
    /// <summary>Single-device bus identity advertised by Android.</summary>
    public const string BusId = "1-1";
    /// <summary>Stop event created by the parent for this launch session.</summary>
    public string StopEvent => $"Local\\StarCoreUsbIpStop-{OwnerPid}-{SessionId}";

    /// <summary>Unambiguous diagnostic endpoint; SDK calls continue to receive the raw host.</summary>
    public string Endpoint => Host.AddressFamily == AddressFamily.InterNetworkV6 ? $"[{Host}]:{Port}" : $"{Host}:{Port}";

    /// <summary>Accept local numeric peers explicitly supplied by the streaming session, without DNS resolution.</summary>
    public static bool TryParse(string[] args, out WatchOptions? options, IEnumerable<IPNetwork>? localPrefixes = null)
    {
        options = null;
        if (args.Length != 4 || (args[0] != "--watch" && args[0] != "--watch-tls") || !IPAddress.TryParse(args[1], out var host)
                || args[1].Contains('[') || args[1].Contains(']')
                || !int.TryParse(args[2], out int pid) || pid <= 0 || !uint.TryParse(args[3], out uint session)) return false;
        byte[] bytes = host.GetAddressBytes();
        bool local;
        if (host.AddressFamily == AddressFamily.InterNetwork) {
            local = host.ToString() == args[1] && (bytes[0] == 10 || (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31)
                    || (bytes[0] == 192 && bytes[1] == 168));
        } else {
            local = (bytes[0] & 0xfe) == 0xfc || (host.IsIPv6LinkLocal && host.ScopeId > 0)
                    || ((bytes[0] & 0xe0) == 0x20 && (args[0] == "--watch-tls" || (localPrefixes ?? LocalPrefixes()).Any(prefix => prefix.Contains(host))));
        }
        if (!local) return false;
        options = new WatchOptions(host, pid, session) { Tls = args[0] == "--watch-tls" };
        return true;
    }

    /// <summary>Enumerate active IPv6 on-link prefixes; never treat a default route as a local network.</summary>
    private static IEnumerable<IPNetwork> LocalPrefixes()
    {
        foreach (var adapter in NetworkInterface.GetAllNetworkInterfaces()) {
            if (adapter.OperationalStatus != OperationalStatus.Up) continue;
            foreach (var local in adapter.GetIPProperties().UnicastAddresses) {
                if (local.Address.AddressFamily != AddressFamily.InterNetworkV6 || local.PrefixLength <= 0) continue;
                byte[] bytes = local.Address.GetAddressBytes();
                for (int bit = local.PrefixLength; bit < 128; ++bit) bytes[bit / 8] &= (byte)~(1 << (7 - bit % 8));
                yield return new IPNetwork(new IPAddress(bytes), local.PrefixLength);
            }
        }
    }
}
