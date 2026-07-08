using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace GUI
{
    /// <summary>
    /// Provides application-specific behavior to supplement the default Application class.
    /// </summary>
    public partial class App : Application
    {
        private MainWindow? _window;

        /// <summary>
        /// Initializes the singleton application object.
        /// </summary>
        public App()
        {
            // Call native bootstrap BEFORE InitializeComponent() triggers the managed one
            EnsureBootstrap();
            InitializeComponent();
        }

        /// <summary>
        /// Initialize Windows App SDK runtime via native bootstrap.
        /// The managed auto-bootstrap fails on some systems, but the native one works.
        /// </summary>
        private static void EnsureBootstrap()
        {
            try
            {
                [DllImport("Microsoft.WindowsAppRuntime.Bootstrap.dll")]
                static extern int MddBootstrapInitialize2(uint majorMinor, string versionTag, uint minVersion);

                int hr = MddBootstrapInitialize2(0x00020002, "", 0);
                if (hr != 0)
                    System.Diagnostics.Debug.WriteLine($"Bootstrap warning: HR=0x{hr:X8}");
            }
            catch (DllNotFoundException)
            {
                System.Diagnostics.Debug.WriteLine("Bootstrap DLL not found (packaged app?)");
            }
            catch { }
        }

        /// <summary>
        /// Invoked when the application is launched.
        /// </summary>
        protected override void OnLaunched(Microsoft.UI.Xaml.LaunchActivatedEventArgs args)
        {
            _window = new MainWindow();
            _window.Closed += OnWindowClosed;
            _window.Activate();

            // Initialize RDMA in the background so the window appears immediately.
            _ = InitializeRdmaAsync();
        }

        private async Task InitializeRdmaAsync()
        {
            if (_window == null)
                return;

            Exception? initError = null;
            try
            {
                RdmaTransfer.Initialize();
                await _window.RefreshAdaptersAsync();
            }
            catch (Exception ex)
            {
                initError = ex;
            }

            // Show error on the UI thread — never on a thread pool thread.
            if (initError != null && _window != null && _window.Content != null)
            {
                _window.DispatcherQueue.TryEnqueue(async () =>
                {
                    try
                    {
                        // Wait for XamlRoot to be ready
                        await Task.Delay(500);
                        var dialog = new ContentDialog
                        {
                            Title = "RDMA 初始化失败",
                            Content = $"无法初始化 RDMA 传输库：{initError.Message}\n\n请确认系统已安装 RDMA 网卡驱动并存在 rdma_transfer.dll。",
                            CloseButtonText = "确定",
                            XamlRoot = _window.Content.XamlRoot
                        };
                        await dialog.ShowAsync();
                    }
                    catch { }
                    _window.Close();
                });
            }
        }

        private void OnWindowClosed(object sender, WindowEventArgs args)
        {
            RdmaTransfer.Cleanup();
        }
    }
}
