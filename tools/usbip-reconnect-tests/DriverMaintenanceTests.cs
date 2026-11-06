using Sunshine.UsbIpReconnect;

/// <summary>Verify that only exact, unbound virtual controller identities are eligible for removal.</summary>
internal static class DriverMaintenanceTests
{
    public static void Run()
    {
        var stale = new DriverNode(@"ROOT\USB\0002", new[] { @"ROOT\USBIP_WIN2\UDE" }, "", "", false);
        var healthy = stale with { Driver = "driver-binding", Service = "usbip2_ude", Started = true };
        void Check(string expected, params DriverNode[] nodes) {
            if (DriverMaintenance.Classify(nodes) != expected) throw new Exception("Incorrect driver state: " + expected);
        }
        Check("missing");
        Check("stale", stale);
        Check("healthy", healthy);
        Check("healthy", stale, healthy);
        Check("error", healthy with { Started = false });
        Check("error", stale with { Service = "bound-service" });
        Check("error", stale with { Driver = "bound-driver" });
        Check("missing", stale with { Id = @"USB\VID_054C\123" });
        Check("missing", stale with { HardwareIds = new[] { @"ROOT\USBIP_WIN2\UDE_OTHER" } });
        Check("stale", stale with { HardwareIds = new[] { @"root\hidmaestro_ude" } });
        if (healthy.Stale || (stale with { Driver = "bound" }).Stale) throw new Exception("Bound controller marked removable");
        foreach (string scenario in new[] { "healthy", "missing", "stale", "rebound", "bound-error", "reboot", "remove-failed", "backup-failed", "prepare-failed", "still-broken" }) {
            DriverNode[] current = scenario == "missing" ? Array.Empty<DriverNode>() : new[] {
                scenario == "healthy" ? healthy : scenario == "bound-error" ? healthy with { Started = false } : stale };
            int backups = 0, removes = 0, prepares = 0;
            bool failed = false;
            int code = -1;
            try {
                code = DriverMaintenance.RunRepair(() => current, _ => {
                    backups++;
                    if (scenario == "backup-failed") throw new IOException("backup failed");
                    if (scenario == "rebound") current = new[] { healthy };
                }, _ => {
                    removes++;
                    return scenario == "reboot" ? 3010 : scenario == "remove-failed" ? 5 : 0;
                }, () => {
                    prepares++;
                    if (scenario == "prepare-failed") throw new IOException("prepare failed");
                    if (scenario != "still-broken") current = new[] { healthy };
                }, _ => {});
            } catch (IOException) { failed = true; }
            bool expectedFailure = new[] { "remove-failed", "backup-failed", "prepare-failed", "still-broken" }.Contains(scenario);
            if (failed != expectedFailure || (scenario == "reboot" && (code != 3010 || prepares != 0))
                    || (scenario == "healthy" && (backups != 0 || removes != 0 || prepares != 0))
                    || ((scenario == "bound-error" || scenario == "rebound") && removes != 0)
                    || (scenario == "stale" && (backups != 1 || removes != 1 || prepares != 1 || code != 0)))
                throw new Exception("Unsafe repair sequence: " + scenario);
        }
        Console.WriteLine("PASS driver-maintenance-policy");
    }
}
