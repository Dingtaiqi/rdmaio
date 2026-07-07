using System;
using System.Runtime.InteropServices;

namespace GUI
{
    /// <summary>
    /// Low-level P/Invoke declarations for rdma_transfer.dll.
    /// </summary>
    internal static class RdmaNative
    {
        private const string DllName = "rdma_transfer.dll";

        #region Lifecycle

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int rdma_transfer_init();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void rdma_transfer_cleanup();

        #endregion

        #region File Transfer

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int rdma_send_file(
            [MarshalAs(UnmanagedType.LPStr)] string remoteIp,
            ushort port,
            [MarshalAs(UnmanagedType.LPWStr)] string filePath);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int rdma_recv_file(
            [MarshalAs(UnmanagedType.LPStr)] string localIp,
            ushort port,
            [MarshalAs(UnmanagedType.LPWStr)] string outputPath);

        #endregion

        #region Benchmark

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int rdma_bench(
            int side,
            [MarshalAs(UnmanagedType.LPStr)] string ip,
            ushort port,
            int sizeMb);

        #endregion

        #region Progress Callback

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void rdma_progress_cb(double percent, double speedMbps, IntPtr userData);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void rdma_set_progress_callback(
            rdma_progress_cb? cb,
            IntPtr userData);

        #endregion

        #region Metadata Callback

        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public delegate void rdma_metadata_cb(
            [MarshalAs(UnmanagedType.LPStr)] string filename,
            ulong fileSize,
            IntPtr userData);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void rdma_set_metadata_callback(
            rdma_metadata_cb? cb,
            IntPtr userData);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void rdma_transfer_cancel();

        #endregion

        #region Error Diagnostics

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr rdma_transfer_last_error();

        #endregion

        #region Adapter Enumeration

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
        public struct rdma_adapter_info
        {
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)]
            public byte[] ip_address;

            public ulong adapter_id;
            public ushort vendor_id;
            public ushort device_id;
            public uint max_transfer_mb;
            public uint max_inline_data;
            public uint max_cq_depth;
            public uint max_initiator_depth;
            public uint flags;
            public int has_in_order_dma;
            public int has_multi_engine;
            public int has_loopback;
        }

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int rdma_list_adapters(
            [Out] rdma_adapter_info[]? info,
            int maxCount);

        #endregion
    }
}
