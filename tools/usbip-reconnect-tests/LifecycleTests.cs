using System.Net;
using Sunshine.UsbIpReconnect;

/// <summary>Exercise real watcher ownership and cancellation with a fake USB transport.</summary>
internal static class LifecycleTests
{
    public static async Task Run()
    {
        if (!WatchOptions.TryParse(new[] { "--watch", "192.168.31.201", "100", "7" }, out var options))
            throw new Exception("Valid session options rejected");
        foreach (var host in new[] { "127.0.0.1", "8.8.8.8", "169.254.1.1", "172.32.0.1", "::1", "192.168.1.999" })
            if (WatchOptions.TryParse(new[] { "--watch", host, "100", "7" }, out _))
                throw new Exception("Unsafe peer accepted: " + host);
        if (WatchOptions.TryParse(new[] { "--watch", "192.168.1.1", "-1", "7" }, out _))
            throw new Exception("Invalid owner accepted");
        Console.WriteLine("PASS session-options");

        var prefixes = new[] { IPNetwork.Parse("2001:db8:1234::/64") };
        foreach (string host in new[] { "fd12::2", "fc00::1", "fe80::1234%7", "2001:db8:1234::2" }) {
            if (!WatchOptions.TryParse(new[] { "--watch", host, "100", "7" }, out var v6, prefixes)
                    || v6!.Endpoint != $"[{host}]:3240") throw new Exception("Local IPv6 rejected: " + host);
        }
        foreach (string host in new[] { "::", "::1", "ff02::1", "fe80::1", "fec0::1", "2001:db8:5678::2",
                "[fd00::1]", "::ffff:192.168.1.1", "::192.168.1.1", "host.local" })
            if (WatchOptions.TryParse(new[] { "--watch", host, "100", "7" }, out _, prefixes))
                throw new Exception("Unsafe IPv6 accepted: " + host);
        Console.WriteLine("PASS ipv6-session-options");

        foreach (var scenario in new[] { "owned", "external", "replacement", "pending" }) {
            using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(4));
            var transport = new FakeTransport(scenario, cancellation);
            var watcher = new ReconnectWatcher(transport, _ => Task.FromResult(true), _ => {}, TimeSpan.FromMilliseconds(1));
            await watcher.Run(cancellation.Token);
            if (scenario == "owned" && (transport.AttachCalls != 1 || transport.DetachCalls != 1 || transport.StopCalls == 0))
                throw new Exception("Owned import was not cleaned up");
            if (scenario == "external" && (transport.AttachCalls != 0 || transport.DetachCalls != 0 || transport.StopCalls != 0))
                throw new Exception("Existing external import was touched");
            if (scenario == "replacement" && transport.DetachCalls != 0)
                throw new Exception("A replacement import was detached");
            if (scenario == "pending" && (transport.StopCalls == 0 || transport.DetachCalls != 0))
                throw new Exception("Pending attach was not cancelled safely");
            Console.WriteLine("PASS lifecycle-" + scenario);
        }
    }

    private sealed class FakeTransport(string scenario, CancellationTokenSource cancellation) : IUsbIpTransport
    {
        public int AttachCalls;
        public int StopCalls;
        public int DetachCalls;
        private int reads;
        public int Find() {
            ++reads;
            if (scenario == "external") { cancellation.Cancel(); return 9; }
            if (scenario == "replacement" && AttachCalls > 0) { cancellation.Cancel(); return 9; }
            if (scenario == "owned" && AttachCalls > 0) return 5;
            return 0;
        }
        public int Attach() {
            ++AttachCalls;
            if (scenario != "replacement") cancellation.Cancel();
            return scenario == "pending" ? 0 : 5;
        }
        public void StopAttachAttempts() => ++StopCalls;
        public void Detach() => ++DetachCalls;
    }
}
