using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Windows.Storage;
using Windows.Storage.Pickers;
using WinRT;
using WinRT.Interop;

namespace GUI
{
    /// <summary>
    /// Main window for the RDMA file transfer GUI.
    /// </summary>
    public sealed partial class MainWindow : Window
    {
        private readonly nint _hwnd;
        private bool _isRunning;
        private CancellationTokenSource? _cts;
        private string? _currentFilePath;
        private ulong? _currentFileSize;
        private Stopwatch? _operationStopwatch;
        private static readonly string _logPath =
            Path.Combine(AppContext.BaseDirectory, "rdma_gui.log");

        private static void Log(string msg)
        {
            string line = $"{DateTime.Now:HH:mm:ss.fff} [{Environment.CurrentManagedThreadId}] {msg}";
            Debug.WriteLine(line);
            try { File.AppendAllText(_logPath, line + Environment.NewLine); }
            catch { /* best-effort */ }
        }

        public MainWindow()
        {
            InitializeComponent();

            _hwnd = WindowNative.GetWindowHandle(this);

            // Set window size.
            this.AppWindow.SetPresenter(Microsoft.UI.Windowing.AppWindowPresenterKind.Default);
            this.AppWindow.Resize(new Windows.Graphics.SizeInt32 { Width = 720, Height = 800 });

            // Default mode: send.
            ModeSendRadioButton.IsChecked = true;
            UpdateUiForMode();

            AdapterComboBox.SelectionChanged += (s, e) => UpdateReceiverIpDisplay();
            BenchmarkSideRadioButtons.SelectionChanged += (s, e) => UpdateBenchmarkIpDisplay();
        }

        private async void RefreshAdaptersButton_Click(object sender, RoutedEventArgs e)
        {
            await RefreshAdaptersAsync();
        }

        internal async Task RefreshAdaptersAsync()
        {
            RefreshAdaptersButton.IsEnabled = false;
            StatusTextBlock.Text = "正在枚举 RDMA 网卡…";
            ErrorTextBlock.Visibility = Visibility.Collapsed;

            try
            {
                IReadOnlyList<RdmaAdapterInfo> adapters = await RdmaTransfer.ListAdaptersAsync();

                AdapterComboBox.ItemsSource = adapters;
                AdapterComboBox.DisplayMemberPath = nameof(RdmaAdapterInfo.DisplayName);

                if (adapters.Count > 0)
                {
                    AdapterComboBox.SelectedIndex = 0;
                    StatusTextBlock.Text = $"找到 {adapters.Count} 个 RDMA 网卡。";
                }
                else
                {
                    StatusTextBlock.Text = "未找到 RDMA 网卡。请确认驱动已安装。";
                }
            }
            catch (Exception ex)
            {
                ShowError($"枚举网卡失败：{ex.Message}");
                StatusTextBlock.Text = "网卡枚举失败。";
            }
            finally
            {
                RefreshAdaptersButton.IsEnabled = true;
            }
        }

        private void ModeRadioButtons_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            UpdateUiForMode();
        }

        private void MemMonSideRadioButtons_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            UpdateMemMonUi();
        }

        private async void MemMonSendButton_Click(object sender, RoutedEventArgs e)
        {
            string text = MemMonInputTextBox.Text;
            if (string.IsNullOrWhiteSpace(text)) return;
            MemMonSendButton.IsEnabled = false;
            try
            {
                await RdmaMemoryShare.WriteStringAsync(0, text);
                StatusTextBlock.Text = $"已写入 {text.Length} 字节到共享内存";
            }
            catch (Exception ex)
            {
                ShowError($"写入失败：{ex.Message}");
            }
            finally
            {
                MemMonSendButton.IsEnabled = true;
            }
        }

        private void UpdateUiForMode()
        {
            ClearFileInfo();
            FilePathTextBox.Text = string.Empty;
            ErrorTextBlock.Visibility = Visibility.Collapsed;

            if (ModeSendRadioButton.IsChecked == true)
            {
                FilePathGrid.Visibility = Visibility.Visible;
                BenchmarkGrid.Visibility = Visibility.Collapsed;
                MemMonGrid.Visibility = Visibility.Collapsed;
                IpTextBox.Visibility = Visibility.Visible;
                ReceiverIpPanel.Visibility = Visibility.Collapsed;
                FilePathTextBox.Header = "要发送的文件";
                FilePathTextBox.PlaceholderText = "点击右侧按钮选择要发送的文件";
                IpTextBox.Header = "目标 IP 地址";
                IpTextBox.PlaceholderText = "例如 192.168.100.2";
                IpTextBox.IsReadOnly = false;
                StartButton.Content = "开始发送";
            }
            else if (ModeReceiveRadioButton.IsChecked == true)
            {
                FilePathGrid.Visibility = Visibility.Visible;
                BenchmarkGrid.Visibility = Visibility.Collapsed;
                MemMonGrid.Visibility = Visibility.Collapsed;
                IpTextBox.Visibility = Visibility.Collapsed;
                ReceiverIpPanel.Visibility = Visibility.Visible;
                FilePathTextBox.Header = "保存目录";
                FilePathTextBox.PlaceholderText = "点击右侧按钮选择接收文件保存目录";
                StartButton.Content = "开始接收";
                UpdateReceiverIpDisplay();
            }
            else if (ModeBenchmarkRadioButton.IsChecked == true)
            {
                FilePathGrid.Visibility = Visibility.Collapsed;
                BenchmarkGrid.Visibility = Visibility.Visible;
                MemMonGrid.Visibility = Visibility.Collapsed;
                IpTextBox.Header = "目标 IP 地址";
                IpTextBox.PlaceholderText = "例如 192.168.100.2";
                IpTextBox.IsReadOnly = false;
                StartButton.Content = "开始测试";
                UpdateBenchmarkIpDisplay();
            }
            else if (ModeMemMonRadioButton.IsChecked == true)
            {
                FilePathGrid.Visibility = Visibility.Collapsed;
                BenchmarkGrid.Visibility = Visibility.Collapsed;
                MemMonGrid.Visibility = Visibility.Visible;
                IpTextBox.Visibility = Visibility.Visible;
                ReceiverIpPanel.Visibility = Visibility.Collapsed;
                IpTextBox.Header = "IP 地址";
                IpTextBox.PlaceholderText = "例如 192.168.100.2";
                IpTextBox.IsReadOnly = false;
                StartButton.Content = "启动监控";
                UpdateMemMonUi();
            }
        }

        private void UpdateMemMonUi()
        {
            bool isWriter = MemMonSideWriterButton.IsChecked == true;
            MemMonWriterGrid.Visibility = isWriter ? Visibility.Visible : Visibility.Collapsed;
            if (isWriter)
            {
                IpTextBox.Visibility = Visibility.Visible;
                ReceiverIpPanel.Visibility = Visibility.Collapsed;
                IpTextBox.Header = "目标 IP 地址";
                StartButton.Content = "连接并写入";
            }
            else
            {
                IpTextBox.Visibility = Visibility.Collapsed;
                ReceiverIpPanel.Visibility = Visibility.Visible;
                UpdateReceiverIpDisplay();
                StartButton.Content = "启动监控";
            }
        }

        private void UpdateReceiverIpDisplay()
        {
            if (AdapterComboBox.SelectedItem is RdmaAdapterInfo adapter)
            {
                ReceiverIpTextBlock.Text = adapter.IpAddress;
            }
            else
            {
                ReceiverIpTextBlock.Text = "未选择网卡";
            }
        }

        private void UpdateBenchmarkIpDisplay()
        {
            if (ModeBenchmarkRadioButton.IsChecked != true)
                return;

            if (BenchmarkSideReceiverRadioButton.IsChecked == true)
            {
                IpTextBox.Visibility = Visibility.Collapsed;
                ReceiverIpPanel.Visibility = Visibility.Visible;
                UpdateReceiverIpDisplay();
            }
            else
            {
                IpTextBox.Visibility = Visibility.Visible;
                ReceiverIpPanel.Visibility = Visibility.Collapsed;
            }
        }

        private async void BrowseButton_Click(object sender, RoutedEventArgs e)
        {
            if (ModeSendRadioButton.IsChecked == true)
            {
                var picker = new FileOpenPicker
                {
                    ViewMode = PickerViewMode.List,
                    SuggestedStartLocation = PickerLocationId.DocumentsLibrary
                };
                picker.FileTypeFilter.Add("*");
                InitializeObject(picker);

                StorageFile? file = await picker.PickSingleFileAsync();
                if (file != null)
                {
                    FilePathTextBox.Text = file.Path;
                    await LoadSenderFileInfoAsync(file.Path);
                }
            }
            else if (ModeReceiveRadioButton.IsChecked == true)
            {
                var picker = new FolderPicker
                {
                    SuggestedStartLocation = PickerLocationId.DocumentsLibrary
                };
                InitializeObject(picker);

                StorageFolder? folder = await picker.PickSingleFolderAsync();
                if (folder != null)
                {
                    FilePathTextBox.Text = folder.Path;
                    ClearFileInfo();
                }
            }
        }

        private async Task LoadSenderFileInfoAsync(string filePath)
        {
            _currentFilePath = filePath;
            _currentFileSize = null;

            var info = new FileInfo(filePath);
            _currentFileSize = info.Exists ? (ulong)info.Length : 0;

            ShowFileInfo(Path.GetFileName(filePath), _currentFileSize, null, null);
            HashTextBlock.Text = "MD5: 计算中…";

            try
            {
                string hash = await FileHashHelper.ComputeMd5Async(filePath);
                _currentFileSize = info.Exists ? (ulong)info.Length : 0;
                ShowFileInfo(Path.GetFileName(filePath), _currentFileSize, hash, null);
            }
            catch (Exception ex)
            {
                HashTextBlock.Text = $"MD5: 计算失败 ({ex.Message})";
                HashTextBlock.Visibility = Visibility.Visible;
            }
        }

        private void InitializeObject(object obj)
        {
            if (obj is null)
                return;

            var initializeWithWindow = obj.As<IInitializeWithWindow>();
            initializeWithWindow.Initialize(_hwnd);
        }

        [ComImport]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        [Guid("3E68D4BD-7135-4D10-8018-9FB6D9F33FA1")]
        private interface IInitializeWithWindow
        {
            void Initialize(nint hwnd);
        }

        private void CancelButton_Click(object sender, RoutedEventArgs e)
        {
            if (_isRunning && _cts != null)
            {
                Log("CANCEL requested");
                StatusTextBlock.Text = "正在取消…";
                RdmaTransfer.Cancel();
                _cts.Cancel();
            }
        }

        private async void StartButton_Click(object sender, RoutedEventArgs e)
        {
            ErrorTextBlock.Visibility = Visibility.Collapsed;
            string? receiveOutputFilePath = null;
            bool operationSucceeded = false;

            // Validate adapter.
            if (AdapterComboBox.SelectedItem is not RdmaAdapterInfo adapter)
            {
                ShowError("请选择一个 RDMA 网卡。");
                return;
            }

            // Determine IP: receiver (file or benchmark) uses selected adapter IP;
            // sender / benchmark sender uses the text box.
            string ip;
            bool isReceiver = ModeReceiveRadioButton.IsChecked == true ||
                              (ModeBenchmarkRadioButton.IsChecked == true &&
                               BenchmarkSideReceiverRadioButton.IsChecked == true);
            if (isReceiver)
            {
                ip = adapter.IpAddress;
            }
            else
            {
                ip = IpTextBox.Text?.Trim() ?? string.Empty;
                if (string.IsNullOrWhiteSpace(ip))
                {
                    ShowError("请输入目标 IP 地址。");
                    return;
                }
            }

            // Validate port.
            if (!ushort.TryParse(PortNumberBox.Text, out ushort port) || port == 0)
            {
                ShowError("请输入有效的端口号（1-65535）。");
                return;
            }

            _cts = new CancellationTokenSource();
            var ct = _cts.Token;

            try
            {
                SetRunning(true);
                TransferProgressBar.Value = 0;
                StatusTextBlock.Text = "正在准备…";
                _operationStopwatch = Stopwatch.StartNew();

                string modeLabel = ModeSendRadioButton.IsChecked == true ? "SEND"
                    : ModeReceiveRadioButton.IsChecked == true ? "RECV" : "BENCH";
                Log($"=== START {modeLabel} ip={ip} port={port} ===");

                var progress = new Progress<RdmaProgress>(p =>
                {
                    TransferProgressBar.Value = Math.Clamp(p.Percent, 0, 100);
                    RateTextBlock.Text = $"速率：{p.SpeedMbps:F1} MB/s";
                    RateTextBlock.Visibility = Visibility.Visible;
                    StatusTextBlock.Text = $"{p.Percent:F1}%";
                });

                if (ModeSendRadioButton.IsChecked == true)
                {
                    string filePath = FilePathTextBox.Text;
                    if (!File.Exists(filePath))
                    {
                        ShowError("要发送的文件不存在。");
                        return;
                    }

                    if (_currentFilePath != filePath)
                    {
                        await LoadSenderFileInfoAsync(filePath);
                    }

                    StatusTextBlock.Text = $"正在发送 {Path.GetFileName(filePath)} 到 {ip}:{port}…";
                    await RdmaTransfer.SendFileAsync(ip, port, filePath, progress, cancellationToken: ct);
                    operationSucceeded = true;

                    double elapsed = _operationStopwatch.Elapsed.TotalSeconds;
                    double avgRate = elapsed > 0 && _currentFileSize.HasValue
                        ? (_currentFileSize.Value / (1024.0 * 1024.0)) / elapsed
                        : 0.0;
                    RateTextBlock.Text = $"平均速率：{avgRate:F1} MB/s";
                    StatusTextBlock.Text = "发送完成。";
                }
                else if (ModeReceiveRadioButton.IsChecked == true)
                {
                    string outputDir = FilePathTextBox.Text;
                    if (string.IsNullOrWhiteSpace(outputDir))
                    {
                        ShowError("请选择保存目录。");
                        return;
                    }

                    if (!Directory.Exists(outputDir))
                    {
                        ShowError("保存目录不存在。");
                        return;
                    }

                    _currentFileSize = null;
                    ClearFileInfo();

                    string? receivedFileName = null;
                    ulong? receivedFileSize = null;

                    // Capture outputDir — don't use FilePathTextBox.Text which may change.
                    string capturedDir = outputDir;
                    var metadataCallback = new Action<string, ulong>((name, size) =>
                    {
                        receivedFileName = name;
                        receivedFileSize = size;
                        receiveOutputFilePath = Path.Combine(capturedDir, name);
                        DispatcherQueue.TryEnqueue(() =>
                        {
                            ShowFileInfo(name, size, null, null);
                            StatusTextBlock.Text = $"正在接收 {name}…";
                        });
                    });

                    StatusTextBlock.Text = $"正在 {ip}:{port} 上等待接收…";
                    await RdmaTransfer.ReceiveFileAsync(ip, port, outputDir, progress, metadataCallback, ct);
                    operationSucceeded = true;

                    string finalFilePath = receivedFileName != null
                        ? Path.Combine(outputDir, receivedFileName)
                        : outputDir;

                    double elapsed = _operationStopwatch.Elapsed.TotalSeconds;
                    double avgRate = elapsed > 0 && receivedFileSize.HasValue
                        ? (receivedFileSize.Value / (1024.0 * 1024.0)) / elapsed
                        : 0.0;
                    RateTextBlock.Text = $"平均速率：{avgRate:F1} MB/s";
                    StatusTextBlock.Text = $"接收完成，已保存到 {finalFilePath}。";

                    if (File.Exists(finalFilePath))
                    {
                        HashTextBlock.Text = "MD5: 计算中…";
                        string hash = await FileHashHelper.ComputeMd5Async(finalFilePath);
                        ShowFileInfo(receivedFileName, receivedFileSize, hash, null);
                    }
                }
                else if (ModeBenchmarkRadioButton.IsChecked == true)
                {
                    if (!int.TryParse(BenchmarkSizeNumberBox.Text, out int sizeMb) || sizeMb <= 0)
                    {
                        ShowError("请输入有效的测试大小（MiB）。");
                        return;
                    }

                    int side = BenchmarkSideSenderRadioButton.IsChecked == true ? 1 : 0;
                    string sideName = side == 1 ? "发送端" : "接收端";

                    ClearFileInfo();
                    StatusTextBlock.Text = $"正在以 {sideName} 运行性能测试…";
                    await RdmaTransfer.RunBenchmarkAsync(side, ip, port, sizeMb, progress, ct);
                    operationSucceeded = true;
                    StatusTextBlock.Text = "性能测试完成。";
                }
                else if (ModeMemMonRadioButton.IsChecked == true)
                {
                    if (!uint.TryParse(MemMonSizeNumberBox.Text, out uint memSize) || memSize < 1024)
                    {
                        ShowError("请输入有效的缓冲区大小（≥1024）。");
                        return;
                    }

                    bool isWriter = MemMonSideWriterButton.IsChecked == true;
                    ClearFileInfo();
                    StatusTextBlock.Text = isWriter
                        ? $"正在连接显示器 {ip}:{port}…"
                        : $"正在 {ip}:{port} 上等待写入器…";

                    await RdmaMemoryShare.StartAsync(isWriter, ip, port, memSize);
                    operationSucceeded = true;
                    Log("MEM: session started");

                    if (isWriter)
                    {
                        // Writer mode — user writes via text box, we just wait
                        StatusTextBlock.Text = $"已连接到 {ip}:{port}，在下方输入数据后点击「写入」";
                        MemMonHexText.Text = $"已连接，缓冲区大小: {RdmaMemoryShare.BufferSize} 字节\n输入数据并点击「写入」以通过 RDMA Write 发送";
                        Log("MEM: writer ready");
                    }
                    else
                    {
                        // Display mode — start update loop
                        Log("MEM: display loop started");
                        await RunDisplayUpdateLoopAsync(ct);
                    }
                }
            }
            catch (OperationCanceledException)
            {
                Log("RESULT=CANCELLED");
                StatusTextBlock.Text = "操作已取消。";
            }
            catch (Exception ex)
            {
                Log($"RESULT=ERROR {ex.Message}");
                ShowError($"操作失败：{ex.Message}");
                StatusTextBlock.Text = "操作失败。";
            }
            finally
            {
                if (operationSucceeded) Log("RESULT=OK");

                // Clean up any partial file left by an interrupted/failed receive.
                if (!operationSucceeded && !string.IsNullOrEmpty(receiveOutputFilePath) && File.Exists(receiveOutputFilePath))
                {
                    try { File.Delete(receiveOutputFilePath); Log($"CLEANUP deleted {receiveOutputFilePath}"); }
                    catch { /* best-effort */ }
                }

                // Memory Monitor cleanup
                if (ModeMemMonRadioButton.IsChecked == true)
                {
                    RdmaMemoryShare.Stop();
                    Log("MEM: cleaned up");
                }

                if (!operationSucceeded)
                {
                    _currentFilePath = null;
                    _currentFileSize = null;
                }

                _cts?.Dispose();
                _cts = null;
                SetRunning(false);
                _operationStopwatch?.Stop();
                Log($"=== DONE ===\n");
            }
        }

        private void ClearFileInfo()
        {
            FileNameTextBlock.Visibility = Visibility.Collapsed;
            FileSizeTextBlock.Visibility = Visibility.Collapsed;
            HashTextBlock.Visibility = Visibility.Collapsed;
            RateTextBlock.Visibility = Visibility.Collapsed;
            TransferProgressBar.Value = 0;
        }

        private void ShowFileInfo(string? fileName, ulong? fileSize, string? hash, string? rate)
        {
            if (!string.IsNullOrEmpty(fileName))
            {
                FileNameTextBlock.Text = $"文件名：{fileName}";
                FileNameTextBlock.Visibility = Visibility.Visible;
            }
            else
            {
                FileNameTextBlock.Visibility = Visibility.Collapsed;
            }

            if (fileSize.HasValue)
            {
                FileSizeTextBlock.Text = $"大小：{FormatFileSize(fileSize.Value)}";
                FileSizeTextBlock.Visibility = Visibility.Visible;
            }
            else
            {
                FileSizeTextBlock.Visibility = Visibility.Collapsed;
            }

            if (!string.IsNullOrEmpty(hash))
            {
                HashTextBlock.Text = $"MD5：{hash}";
                HashTextBlock.Visibility = Visibility.Visible;
            }
            else if (HashTextBlock.Text != "MD5: 计算中…")
            {
                HashTextBlock.Visibility = Visibility.Collapsed;
            }

            if (!string.IsNullOrEmpty(rate))
            {
                RateTextBlock.Text = rate;
                RateTextBlock.Visibility = Visibility.Visible;
            }
        }

        private static string FormatFileSize(ulong bytes)
        {
            string[] units = { "B", "KB", "MB", "GB", "TB" };
            double size = bytes;
            int unitIndex = 0;
            while (size >= 1024.0 && unitIndex < units.Length - 1)
            {
                size /= 1024.0;
                unitIndex++;
            }
            return $"{size:F2} {units[unitIndex]}";
        }

        /// <summary>
        /// Display-side update loop: waits for doorbells and refreshes the hex view.
        /// </summary>
        private async Task RunDisplayUpdateLoopAsync(CancellationToken ct)
        {
            try
            {
                int updateCount = 0;
                while (!ct.IsCancellationRequested)
                {
                    bool updated = await RdmaMemoryShare.WaitForUpdateAsync(1000, ct);
                    if (ct.IsCancellationRequested) break;
                    if (updated)
                    {
                        var (off, len) = RdmaMemoryShare.GetLastWriteInfo();
                        updateCount++;
                        Log($"MEM: update #{updateCount} offset={off} length={len}");

                        DispatcherQueue.TryEnqueue(() =>
                        {
                            MemMonHexText.Text = FormatHexDump(RdmaMemoryShare.BufferPtr,
                                                               RdmaMemoryShare.BufferSize, 0, 256);
                            MemMonStatusText.Text = $"更新 #{updateCount} | 偏移={off} | 长度={len} "
                                + $"| 缓冲区={RdmaMemoryShare.BufferSize} 字节";
                            TransferProgressBar.Value = 100;
                        });
                    }
                    else
                    {
                        // Timeout — heartbeat to keep UI responsive
                        if (updateCount > 0)
                        {
                            DispatcherQueue.TryEnqueue(() =>
                            {
                                MemMonHexText.Text = FormatHexDump(RdmaMemoryShare.BufferPtr,
                                                                   RdmaMemoryShare.BufferSize, 0, 256);
                            });
                        }
                    }
                }
            }
            catch (OperationCanceledException) { }
            catch (Exception ex)
            {
                Log($"MEM: display loop error: {ex.Message}");
                DispatcherQueue.TryEnqueue(() => ShowError($"监控循环错误：{ex.Message}"));
            }
            finally
            {
                Log("MEM: display loop ended");
            }
        }

        /// <summary>
        /// Format a hex dump from a native pointer, showing offset + hex bytes + ASCII.
        /// </summary>
        private static string FormatHexDump(IntPtr ptr, uint totalSize, uint startOffset, uint maxBytes)
        {
            if (ptr == IntPtr.Zero || totalSize == 0) return "(无数据)";

            uint count = Math.Min(maxBytes, totalSize - startOffset);
            if (count == 0) return "(偏移超出范围)";

            byte[] buf = new byte[count];
            Marshal.Copy(ptr + (int)startOffset, buf, 0, (int)count);

            var sb = new System.Text.StringBuilder((int)(count / 16 * 80 + 100));
            sb.AppendLine($"共享内存 | 共 {totalSize} 字节 | 显示前 {count} 字节");
            sb.AppendLine(new string('-', 80));

            int rows = (int)((count + 15) / 16);
            for (int r = 0; r < rows; r++)
            {
                int offset = r * 16;
                sb.Append($"{startOffset + (uint)offset:X8}  ");

                // Hex bytes
                for (int c = 0; c < 16; c++)
                {
                    int idx = offset + c;
                    if (idx < count)
                        sb.Append($"{buf[idx]:X2} ");
                    else
                        sb.Append("   ");
                    if (c == 7) sb.Append(" ");
                }

                sb.Append(" |");

                // ASCII
                for (int c = 0; c < 16; c++)
                {
                    int idx = offset + c;
                    if (idx < count)
                    {
                        byte b = buf[idx];
                        sb.Append(b >= 32 && b < 127 ? (char)b : '.');
                    }
                    else
                    {
                        sb.Append(' ');
                    }
                }

                sb.AppendLine("|");
            }

            return sb.ToString();
        }

        private void ShowError(string message)
        {
            ErrorTextBlock.Text = message;
            ErrorTextBlock.Visibility = Visibility.Visible;
        }

        private void SetRunning(bool running)
        {
            _isRunning = running;
            StartButton.IsEnabled = !running;
            CancelButton.IsEnabled = running;

            bool enabled = !running;
            AdapterComboBox.IsEnabled = enabled;
            RefreshAdaptersButton.IsEnabled = enabled;
            ModeSendRadioButton.IsEnabled = enabled;
            ModeReceiveRadioButton.IsEnabled = enabled;
            ModeBenchmarkRadioButton.IsEnabled = enabled;
            IpTextBox.IsEnabled = enabled;
            PortNumberBox.IsEnabled = enabled;
            FilePathTextBox.IsEnabled = enabled;
            BrowseButton.IsEnabled = enabled;
            BenchmarkSizeNumberBox.IsEnabled = enabled;
            BenchmarkSideSenderRadioButton.IsEnabled = enabled;
            BenchmarkSideReceiverRadioButton.IsEnabled = enabled;
        }
    }
}
