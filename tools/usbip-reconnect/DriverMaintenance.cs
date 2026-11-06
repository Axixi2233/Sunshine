using Microsoft.Win32;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Principal;

namespace Sunshine.UsbIpReconnect;

/// <summary>Verified USB/IP root controller identity and binding; no arbitrary device IDs are accepted.</summary>
internal sealed record DriverNode(string Id, string[] HardwareIds, string Driver, string Service, bool Started)
{
    /// <summary>Match only the supported virtual root controller's exact hardware identity.</summary>
    public bool Recognized => Id.StartsWith("ROOT\\", StringComparison.OrdinalIgnoreCase)
        && HardwareIds.Any(id => id.Equals("ROOT\\USBIP_WIN2\\UDE", StringComparison.OrdinalIgnoreCase)
            || id.Equals("ROOT\\HIDMAESTRO_UDE", StringComparison.OrdinalIgnoreCase));
    /// <summary>Only a node with neither a driver nor a service binding can be removed.</summary>
    public bool Stale => Recognized && Driver.Length == 0 && Service.Length == 0;
    /// <summary>A started, bound controller needs no repair.</summary>
    public bool Healthy => Recognized && Driver.Length != 0 && Service.Length != 0 && Started;
}

/// <summary>Inspect without elevation; repair only verified unbound virtual controllers using the bundled SDK.</summary>
internal static class DriverMaintenance
{
    /// <summary>Derive a stable UI state without treating PnP problem-code zero as a driver binding.</summary>
    public static string Classify(IEnumerable<DriverNode> nodes)
    {
        var controllers = nodes.Where(node => node.Recognized).ToArray();
        if (controllers.Any(node => node.Healthy)) return "healthy";
        if (controllers.Length == 0) return "missing";
        return controllers.All(node => node.Stale) ? "stale" : "error";
    }

    /// <summary>Read root-device bindings and their actual devnode started state.</summary>
    public static DriverNode[] Inspect()
    {
        var result = new List<DriverNode>();
        using var root = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Enum\ROOT")
            ?? throw new IOException("Cannot inspect root devices");
        foreach (string group in root.GetSubKeyNames()) {
            using var parent = root.OpenSubKey(group);
            if (parent is null) continue;
            foreach (string instance in parent.GetSubKeyNames()) {
                using var device = parent.OpenSubKey(instance);
                if (device is null) continue;
                string id = $"ROOT\\{group}\\{instance}";
                var node = new DriverNode(id, device.GetValue("HardwareID") as string[] ?? Array.Empty<string>(),
                    device.GetValue("Driver") as string ?? "", device.GetValue("Service") as string ?? "", false);
                if (!node.Recognized) continue;
                bool started = CM_Locate_DevNodeW(out uint devnode, id, 0) == 0
                    && CM_Get_DevNode_Status(out uint status, out uint problem, devnode, 0) == 0
                    && (status & 8) != 0 && problem == 0;
                result.Add(node with { Started = started });
            }
        }
        return result.ToArray();
    }

    /// <summary>Repair an idle host. Exit 3010 means Windows explicitly requested a reboot.</summary>
    public static int Repair(Action<string> log)
    {
        using var identity = WindowsIdentity.GetCurrent();
        if (!new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator)) return 5;
        using var mutex = new Mutex(false, @"Local\StarCoreUsbIpDriverMaintenance");
        bool acquired;
        try { acquired = mutex.WaitOne(0); } catch (AbandonedMutexException) { acquired = true; }
        if (!acquired) return 170;
        try {
            return RunRepair(Inspect, node => {
                string backup = Path.Combine(AppContext.BaseDirectory, "..", "config", "usbip-driver-backup",
                    DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fffffff") + ".reg");
                Directory.CreateDirectory(Path.GetDirectoryName(backup)!);
                int saved = RunSystemTool("reg.exe", new[] { "export", @"HKLM\SYSTEM\CurrentControlSet\Enum\" + node.Id, Path.GetFullPath(backup), "/y" });
                if (saved != 0) throw new IOException("Could not back up the unbound USB/IP controller");
            }, node => RunSystemTool("pnputil.exe", new[] { "/remove-device", node.Id }), () => SdkTransport.Prepare(log), log);
        } finally { mutex.ReleaseMutex(); }
    }

    /// <summary>Execute the repair policy through testable device operations, preserving healthy and rebound nodes.</summary>
    internal static int RunRepair(Func<DriverNode[]> inspect, Action<DriverNode> backup, Func<DriverNode, int> remove, Action prepare, Action<string> log)
    {
        var nodes = inspect();
        if (Classify(nodes) == "healthy") return 0;
        foreach (var node in nodes.Where(node => node.Stale)) {
            backup(node);
            // Re-read after backup, immediately before removal. Rebound devices stay untouched.
            if (!inspect().Any(current => current.Id == node.Id && current.Stale)) continue;
            log("Removing verified unbound USB/IP controller " + node.Id);
            int removed = remove(node);
            if (removed == 3010) return 3010;
            if (removed != 0) throw new IOException("Windows refused stale-controller removal: " + removed);
        }
        prepare();
        if (Classify(inspect()) != "healthy") throw new IOException("USB/IP controller is still unavailable after repair");
        log("USB/IP driver repair completed");
        return 0;
    }

    /// <summary>Wait for fixed Windows utilities to finish so pending PnP operations cannot race a retry.</summary>
    private static int RunSystemTool(string tool, string[] arguments)
    {
        var start = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), tool)) {
            UseShellExecute = false, CreateNoWindow = true,
        };
        foreach (string argument in arguments) start.ArgumentList.Add(argument);
        using var process = Process.Start(start) ?? throw new IOException("Cannot start Windows driver utility");
        process.WaitForExit();
        return process.ExitCode;
    }

    /// <summary>Find only present devnodes; phantom registry entries are not considered healthy.</summary>
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern uint CM_Locate_DevNodeW(out uint devnode, string id, uint flags);
    /// <summary>Read the kernel PnP status and problem code.</summary>
    [DllImport("cfgmgr32.dll", ExactSpelling = true)]
    private static extern uint CM_Get_DevNode_Status(out uint status, out uint problem, uint devnode, uint flags);
}
