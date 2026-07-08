using System;

namespace GUI
{
    /// <summary>
    /// Progress update reported by the RDMA transfer layer.
    /// </summary>
    public sealed class RdmaProgress : EventArgs
    {
        public RdmaProgress(double percent, double speedMbps, string? status = null)
        {
            Percent = percent;
            SpeedMbps = speedMbps;
            Status = status;
        }

        /// <summary>
        /// Completion percentage in the range [0, 100].
        /// </summary>
        public double Percent { get; }

        /// <summary>
        /// Current throughput in MB/s.
        /// </summary>
        public double SpeedMbps { get; }

        /// <summary>
        /// Optional status text.
        /// </summary>
        public string? Status { get; }
    }
}
