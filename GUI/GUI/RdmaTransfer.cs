using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace GUI
{
    /// <summary>
    /// High-level asynchronous wrapper around the rdma_transfer.dll C API.
    /// </summary>
    public static class RdmaTransfer
    {
        private static int _initCount = 0;
        private static GCHandle? _progressHandle;
        private static GCHandle? _metadataHandle;

        private static readonly RdmaNative.rdma_progress_cb ProgressCallback = (percent, speedMbps, userData) =>
        {
            var handle = GCHandle.FromIntPtr(userData);
            if (handle.Target is IProgress<RdmaProgress> progress)
            {
                progress.Report(new RdmaProgress(percent, speedMbps));
            }
        };

        private static readonly RdmaNative.rdma_metadata_cb MetadataCallback = (filename, fileSize, userData) =>
        {
            var handle = GCHandle.FromIntPtr(userData);
            if (handle.Target is Action<string, ulong> callback)
            {
                callback(filename, fileSize);
            }
        };

        /// <summary>
        /// Initializes the RDMA transfer library. Safe to call multiple times.
        /// </summary>
        public static void Initialize()
        {
            if (Interlocked.Increment(ref _initCount) == 1)
            {
                if (RdmaNative.rdma_transfer_init() != 0)
                {
                    Interlocked.Decrement(ref _initCount);
                    throw new InvalidOperationException("Failed to initialize rdma_transfer library.");
                }
            }
        }

        /// <summary>
        /// Cleans up the RDMA transfer library. Safe to call multiple times.
        /// </summary>
        public static void Cleanup()
        {
            if (Interlocked.Decrement(ref _initCount) == 0)
            {
                SetProgressCallback(null);
                RdmaNative.rdma_transfer_cleanup();
            }
        }

        /// <summary>
        /// Enumerates RDMA-capable adapters on the system.
        /// </summary>
        public static Task<List<RdmaAdapterInfo>> ListAdaptersAsync(CancellationToken cancellationToken = default)
        {
            return Task.Run(() =>
            {
                cancellationToken.ThrowIfCancellationRequested();

                int count = RdmaNative.rdma_list_adapters(null, 0);
                if (count < 0)
                {
                    throw new InvalidOperationException("Failed to enumerate RDMA adapters.");
                }
                if (count == 0)
                {
                    return new List<RdmaAdapterInfo>();
                }

                var adapters = new RdmaNative.rdma_adapter_info[count];
                int filled = RdmaNative.rdma_list_adapters(adapters, count);
                if (filled < 0)
                {
                    throw new InvalidOperationException("Failed to retrieve RDMA adapter details.");
                }

                var result = new List<RdmaAdapterInfo>(filled);
                for (int i = 0; i < filled; i++)
                {
                    var info = adapters[i];
                    string ip = Encoding.ASCII.GetString(info.ip_address).TrimEnd('\0');
                    result.Add(new RdmaAdapterInfo(
                        ip,
                        info.adapter_id,
                        info.vendor_id,
                        info.device_id,
                        info.max_transfer_mb,
                        info.max_inline_data,
                        info.max_cq_depth,
                        info.max_initiator_depth,
                        info.flags,
                        info.has_in_order_dma != 0,
                        info.has_multi_engine != 0,
                        info.has_loopback != 0));
                }

                return result;
            }, cancellationToken);
        }

        /// <summary>
        /// Sends a file to a remote peer. The operation is performed on a thread-pool thread.
        /// </summary>
        public static Task SendFileAsync(
            string remoteIp,
            ushort port,
            string filePath,
            IProgress<RdmaProgress>? progress = null,
            Action<string, ulong>? metadataCallback = null,
            CancellationToken cancellationToken = default)
        {
            if (string.IsNullOrWhiteSpace(remoteIp))
                throw new ArgumentException("Remote IP is required.", nameof(remoteIp));
            if (string.IsNullOrWhiteSpace(filePath))
                throw new ArgumentException("File path is required.", nameof(filePath));

            return Task.Run(() =>
            {
                cancellationToken.ThrowIfCancellationRequested();
                SetProgressCallback(progress);
                SetMetadataCallback(metadataCallback);
                using var reg = cancellationToken.Register(RdmaNative.rdma_transfer_cancel);
                try
                {
                    int ret = RdmaNative.rdma_send_file(remoteIp, port, filePath);
                    cancellationToken.ThrowIfCancellationRequested();
                    if (ret != 0)
                    {
                        throw new InvalidOperationException($"rdma_send_file failed with code {ret}.");
                    }
                }
                finally
                {
                    SetProgressCallback(null);
                    SetMetadataCallback(null);
                }
            }, cancellationToken);
        }

        /// <summary>
        /// Receives a file from a remote peer. The operation is performed on a thread-pool thread.
        /// </summary>
        public static Task ReceiveFileAsync(
            string localIp,
            ushort port,
            string outputPath,
            IProgress<RdmaProgress>? progress = null,
            Action<string, ulong>? metadataCallback = null,
            CancellationToken cancellationToken = default)
        {
            if (string.IsNullOrWhiteSpace(localIp))
                throw new ArgumentException("Local IP is required.", nameof(localIp));
            if (string.IsNullOrWhiteSpace(outputPath))
                throw new ArgumentException("Output path is required.", nameof(outputPath));

            return Task.Run(() =>
            {
                cancellationToken.ThrowIfCancellationRequested();
                SetProgressCallback(progress);
                SetMetadataCallback(metadataCallback);
                using var reg = cancellationToken.Register(RdmaNative.rdma_transfer_cancel);
                try
                {
                    int ret = RdmaNative.rdma_recv_file(localIp, port, outputPath);
                    cancellationToken.ThrowIfCancellationRequested();
                    if (ret != 0)
                    {
                        throw new InvalidOperationException($"rdma_recv_file failed with code {ret}.");
                    }
                }
                finally
                {
                    SetProgressCallback(null);
                    SetMetadataCallback(null);
                }
            }, cancellationToken);
        }

        /// <summary>
        /// Runs a memory-to-memory RDMA Write throughput test.
        /// </summary>
        /// <param name="side">0 = receiver, 1 = sender.</param>
        public static Task RunBenchmarkAsync(
            int side,
            string ip,
            ushort port,
            int sizeMb,
            IProgress<RdmaProgress>? progress = null,
            CancellationToken cancellationToken = default)
        {
            if (side != 0 && side != 1)
                throw new ArgumentException("Side must be 0 (receiver) or 1 (sender).", nameof(side));
            if (string.IsNullOrWhiteSpace(ip))
                throw new ArgumentException("IP address is required.", nameof(ip));
            if (sizeMb <= 0)
                throw new ArgumentException("Benchmark size must be positive.", nameof(sizeMb));

            return Task.Run(() =>
            {
                cancellationToken.ThrowIfCancellationRequested();
                SetProgressCallback(progress);
                using var reg = cancellationToken.Register(RdmaNative.rdma_transfer_cancel);
                try
                {
                    int ret = RdmaNative.rdma_bench(side, ip, port, sizeMb);
                    cancellationToken.ThrowIfCancellationRequested();
                    if (ret != 0)
                    {
                        throw new InvalidOperationException($"rdma_bench failed with code {ret}.");
                    }
                }
                finally
                {
                    SetProgressCallback(null);
                }
            }, cancellationToken);
        }

        /// <summary>
        /// Cancels an ongoing transfer or benchmark. Safe to call from any thread.
        /// </summary>
        public static void Cancel()
        {
            RdmaNative.rdma_transfer_cancel();
        }

        private static void SetProgressCallback(IProgress<RdmaProgress>? progress)
        {
            // Clear any existing callback first.
            RdmaNative.rdma_set_progress_callback(null, IntPtr.Zero);

            if (_progressHandle.HasValue)
            {
                _progressHandle.Value.Free();
                _progressHandle = null;
            }

            if (progress != null)
            {
                var handle = GCHandle.Alloc(progress);
                _progressHandle = handle;
                RdmaNative.rdma_set_progress_callback(
                    ProgressCallback,
                    GCHandle.ToIntPtr(handle));
            }
        }

        private static void SetMetadataCallback(Action<string, ulong>? callback)
        {
            RdmaNative.rdma_set_metadata_callback(null, IntPtr.Zero);

            if (_metadataHandle.HasValue)
            {
                _metadataHandle.Value.Free();
                _metadataHandle = null;
            }

            if (callback != null)
            {
                var handle = GCHandle.Alloc(callback);
                _metadataHandle = handle;
                RdmaNative.rdma_set_metadata_callback(
                    MetadataCallback,
                    GCHandle.ToIntPtr(handle));
            }
        }
    }
}
