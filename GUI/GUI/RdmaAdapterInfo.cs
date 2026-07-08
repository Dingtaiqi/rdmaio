using System;
using System.Net;
using System.Text;

namespace GUI
{
    /// <summary>
    /// Information about one RDMA-capable network adapter.
    /// </summary>
    public sealed class RdmaAdapterInfo
    {
        public RdmaAdapterInfo(
            string ipAddress,
            ulong adapterId,
            ushort vendorId,
            ushort deviceId,
            uint maxTransferMb,
            uint maxInlineData,
            uint maxCqDepth,
            uint maxInitiatorDepth,
            uint flags,
            bool hasInOrderDma,
            bool hasMultiEngine,
            bool hasLoopback)
        {
            IpAddress = ipAddress;
            AdapterId = adapterId;
            VendorId = vendorId;
            DeviceId = deviceId;
            MaxTransferMb = maxTransferMb;
            MaxInlineData = maxInlineData;
            MaxCqDepth = maxCqDepth;
            MaxInitiatorDepth = maxInitiatorDepth;
            Flags = flags;
            HasInOrderDma = hasInOrderDma;
            HasMultiEngine = hasMultiEngine;
            HasLoopback = hasLoopback;
        }

        public string IpAddress { get; }
        public ulong AdapterId { get; }
        public ushort VendorId { get; }
        public ushort DeviceId { get; }
        public uint MaxTransferMb { get; }
        public uint MaxInlineData { get; }
        public uint MaxCqDepth { get; }
        public uint MaxInitiatorDepth { get; }
        public uint Flags { get; }
        public bool HasInOrderDma { get; }
        public bool HasMultiEngine { get; }
        public bool HasLoopback { get; }

        /// <summary>
        /// User-friendly display string for the adapter combo box.
        /// </summary>
        public string DisplayName
        {
            get
            {
                var sb = new StringBuilder();
                sb.Append(IpAddress);
                sb.Append($" (Vendor:0x{VendorId:X4} Device:0x{DeviceId:X4})");
                return sb.ToString();
            }
        }

        /// <summary>
        /// Detailed tooltip/description of the adapter capabilities.
        /// </summary>
        public string Description
        {
            get
            {
                var sb = new StringBuilder();
                sb.AppendLine($"Adapter ID: 0x{AdapterId:X16}");
                sb.AppendLine($"Max transfer: {MaxTransferMb} MB");
                sb.AppendLine($"Max inline data: {MaxInlineData} B");
                sb.AppendLine($"Max CQ depth: {MaxCqDepth}");
                sb.AppendLine($"Max initiator depth: {MaxInitiatorDepth}");
                sb.AppendLine($"Flags: 0x{Flags:X8}");
                sb.Append($"In-order DMA: {(HasInOrderDma ? "yes" : "no")}, Multi-engine: {(HasMultiEngine ? "yes" : "no")}, Loopback: {(HasLoopback ? "yes" : "no")}");
                return sb.ToString();
            }
        }
    }
}
