using System;
using System.Runtime.InteropServices;

namespace GUI;

/// <summary>
/// Custom entry point that calls the native Windows App SDK bootstrap
/// before initializing WinUI, bypassing the broken managed auto-initializer.
/// </summary>
static class Program
{
    [DllImport("Microsoft.WindowsAppRuntime.Bootstrap.dll")]
    private static extern int MddBootstrapInitialize2(uint majorMinor, string versionTag, uint minVersion);

    [DllImport("Microsoft.WindowsAppRuntime.Bootstrap.dll")]
    private static extern void MddBootstrapShutdown();

    [STAThread]
    static int Main(string[] args)
    {
        // Step 1: Initialize Windows App SDK native bootstrap BEFORE any WinUI code.
        int hr = MddBootstrapInitialize2(0x00020002, "", 0);
        if (hr != 0)
        {
            string msg = $"Windows App SDK bootstrap failed (HR=0x{hr:X8}).\n"
                       + "The application cannot start without the Windows App Runtime.\n"
                       + "Please install Microsoft.WindowsAppRuntime.2 from the Microsoft Store.";
            _ = MessageBox(IntPtr.Zero, msg, "RDMA 传输工具", 0x10 /* MB_ICONERROR */);
            return hr;
        }

        // Step 2: Initialize WinUI COM wrappers
        global::WinRT.ComWrappersSupport.InitializeComWrappers();

        // Step 3: Start the WinUI application
        global::Microsoft.UI.Xaml.Application.Start((p) =>
        {
            var context = new global::Microsoft.UI.Dispatching.DispatcherQueueSynchronizationContext(
                global::Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread());
            global::System.Threading.SynchronizationContext.SetSynchronizationContext(context);
            new App();
        });

        // Step 4: Cleanup bootstrap on shutdown
        MddBootstrapShutdown();
        return 0;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int MessageBox(IntPtr hWnd, string text, string caption, uint type);
}
