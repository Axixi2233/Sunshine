using System.Reflection;
using HIDMaestro;

namespace Sunshine.UsbIpReconnect;

/// <summary>Version-pinned access to the embedded SDK's transport without a development DLL path.</summary>
internal sealed class SdkTransport : IUsbIpTransport
{
    private static Assembly Sdk => typeof(HMContext).Assembly;
    private readonly object[] location;
    private readonly MethodInfo find;
    private readonly MethodInfo attach;
    private readonly MethodInfo stop;
    private readonly MethodInfo detach;

    /// <summary>Resolve transport operations against the embedded, pinned SDK.</summary>
    public SdkTransport(WatchOptions options)
    {
        var type = Sdk.GetType("HIDMaestro.Internal.Usbip.VhciClient", throwOnError: true)!;
        Type[] signature = { typeof(string), typeof(int), typeof(string) };
        location = new object[] { options.Host.ToString(), WatchOptions.Port, WatchOptions.BusId };
        find = Method(type, "FindLocation", signature);
        attach = Method(type, "Attach", signature);
        stop = Method(type, "StopAttachAttempts", signature);
        detach = Method(type, "DetachLocation", signature);
    }

    /// <summary>Install or update the transport using the SDK's existing shared-driver policy.</summary>
    public static void Prepare(Action<string> log)
    {
        var installer = Sdk.GetType("HIDMaestro.Internal.Usbip.UsbipDriverInstaller", throwOnError: true)!;
        var ensure = Method(installer, "EnsureInstalled", new[] { typeof(Action<string>) });
        if (ensure.Invoke(null, new object[] { log }) is not true)
            throw new IOException("USB/IP host controller is unavailable; run StarCore as administrator to prepare its driver.");
    }

    /// <summary>Find the exact export.</summary>
    public int Find() => (int)find.Invoke(null, location)!;
    /// <summary>Attach the exact export.</summary>
    public int Attach() => (int)attach.Invoke(null, location)!;
    /// <summary>Cancel this export's pending imports.</summary>
    public void StopAttachAttempts() => stop.Invoke(null, location);
    /// <summary>Detach this export after the watcher verified ownership.</summary>
    public void Detach() => detach.Invoke(null, location);

    /// <summary>Resolve an exact pinned SDK signature before any USB device is imported.</summary>
    private static MethodInfo Method(Type type, string name, Type[] signature) =>
        type.GetMethod(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static, signature)
            ?? throw new MissingMethodException(type.FullName, name);
}
