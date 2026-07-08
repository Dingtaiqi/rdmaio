using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

namespace GUI
{
    /// <summary>
    /// High-level wrapper around rdma_mem_* native API for real-time shared memory display.
    ///
    /// Display side (mode=0): allocates buffer, sender RDMA Writes into it,
    ///                        we see updates via doorbell notifications.
    /// Writer side  (mode=1): RDMA Writes data into display's buffer.
    /// </summary>
    public static class RdmaMemoryShare
    {
        /// <summary>Get the shared buffer pointer as IntPtr. Only valid on display side.</summary>
        public static IntPtr BufferPtr { get; private set; } = IntPtr.Zero;

        /// <summary>Size of the shared buffer in bytes.</summary>
        public static uint BufferSize { get; private set; } = 0;

        private static int _mode = -1; // 0=display, 1=writer, -1=not started

        /// <summary>
        /// Start a shared memory session.
        /// </summary>
        /// <param name="isWriter">true = writer (sends data), false = display (receives/render)</param>
        public static async Task StartAsync(bool isWriter, string ip, ushort port, uint sizeBytes)
        {
            await Task.Run(() =>
            {
                int mode = isWriter ? 1 : 0;
                int ret = RdmaNative.rdma_mem_start(mode, ip, port, sizeBytes);
                if (ret != 0)
                {
                    string err = Marshal.PtrToStringAnsi(RdmaNative.rdma_transfer_last_error()) ?? "unknown";
                    throw new InvalidOperationException($"Memory share failed: {err}");
                }

                _mode = mode;

                if (!isWriter)
                {
                    BufferPtr = RdmaNative.rdma_mem_buffer();
                    BufferSize = RdmaNative.rdma_mem_size();
                    if (BufferPtr == IntPtr.Zero)
                        throw new InvalidOperationException("Got null buffer from display");
                }
            });
        }

        /// <summary>
        /// Writer: write data to the shared buffer at given offset.
        /// Data is RDMA Written to the display side and a doorbell is sent.
        /// </summary>
        public static Task WriteAsync(uint offset, byte[] data, int index, int count)
        {
            return Task.Run(() =>
            {
                if (_mode != 1) throw new InvalidOperationException("Not in writer mode");
                unsafe
                {
                    fixed (byte* p = data)
                    {
                        int ret = RdmaNative.rdma_mem_write(offset, (IntPtr)(p + index), (uint)count);
                        if (ret != 0)
                        {
                            string err = Marshal.PtrToStringAnsi(RdmaNative.rdma_transfer_last_error()) ?? "unknown";
                            throw new InvalidOperationException($"Write failed: {err}");
                        }
                    }
                }
            });
        }

        /// <summary>
        /// Writer: write a string to the shared buffer (converted to UTF-8 bytes).
        /// </summary>
        public static Task WriteStringAsync(uint offset, string text)
        {
            byte[] bytes = System.Text.Encoding.UTF8.GetBytes(text);
            return WriteAsync(offset, bytes, 0, bytes.Length);
        }

        /// <summary>
        /// Display: wait for the next data update from the writer.
        /// Blocks until data arrives or timeout.
        /// </summary>
        /// <param name="timeoutMs">Timeout in ms (0 = infinite)</param>
        /// <returns>true if new data arrived, false on timeout</returns>
        public static async Task<bool> WaitForUpdateAsync(int timeoutMs, CancellationToken ct = default)
        {
            if (_mode != 0) throw new InvalidOperationException("Not in display mode");

            // We need to poll because rdma_mem_wait is a blocking native call
            // that can't be cancelled during the wait. Run it on a thread pool thread.
            var tcs = new TaskCompletionSource<int>();

            using var reg = ct.Register(() => tcs.TrySetCanceled());

            var threadTask = Task.Run(() =>
            {
                int ret = RdmaNative.rdma_mem_wait(timeoutMs);
                tcs.TrySetResult(ret);
            });

            int result = await tcs.Task;
            return result == 1;
        }

        /// <summary>
        /// Display: get info about the last update (offset + length).
        /// </summary>
        public static (uint offset, uint length) GetLastWriteInfo()
        {
            RdmaNative.rdma_mem_last_write(out uint off, out uint len);
            return (off, len);
        }

        /// <summary>
        /// Stop the memory share session.
        /// </summary>
        public static void Stop()
        {
            if (_mode >= 0)
            {
                RdmaNative.rdma_mem_stop();
                _mode = -1;
                BufferPtr = IntPtr.Zero;
                BufferSize = 0;
            }
        }

        /// <summary>
        /// Read a portion of the shared buffer as a byte array.
        /// </summary>
        public static byte[] ReadBuffer(uint offset, uint count)
        {
            if (BufferPtr == IntPtr.Zero) return Array.Empty<byte>();
            if (offset + count > BufferSize) count = BufferSize - offset;
            byte[] result = new byte[count];
            Marshal.Copy(BufferPtr + (int)offset, result, 0, (int)count);
            return result;
        }
    }
}
