using System;
using System.IO;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;

namespace GUI
{
    /// <summary>
    /// Computes file hashes.  Uses MD5 by default because it is faster than SHA256
    /// for large file integrity checks in this local RDMA scenario.
    /// </summary>
    public static class FileHashHelper
    {
        /// <summary>
        /// Computes the MD5 hash of a file and returns it as a lowercase hex string.
        /// </summary>
        public static Task<string> ComputeMd5Async(string filePath, IProgress<double>? progress = null, CancellationToken cancellationToken = default)
        {
            return Task.Run(() =>
            {
                cancellationToken.ThrowIfCancellationRequested();

                using var md5 = MD5.Create();
                using var stream = new FileStream(filePath, FileMode.Open, FileAccess.Read, FileShare.Read, 81920, FileOptions.SequentialScan);

                long totalBytes = stream.Length;
                long processedBytes = 0;
                byte[] buffer = new byte[81920];
                int bytesRead;

                while ((bytesRead = stream.Read(buffer, 0, buffer.Length)) > 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    md5.TransformBlock(buffer, 0, bytesRead, null, 0);
                    processedBytes += bytesRead;
                    if (totalBytes > 0)
                    {
                        progress?.Report((double)processedBytes / totalBytes * 100.0);
                    }
                }

                md5.TransformFinalBlock(buffer, 0, 0);
                return BitConverter.ToString(md5.Hash!).Replace("-", string.Empty).ToLowerInvariant();
            }, cancellationToken);
        }
    }
}
