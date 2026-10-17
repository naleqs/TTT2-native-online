// Xbox 360 disc image (XDVDFS) reader used by setup.ps1. Compiled at setup time with
// PowerShell's Add-Type, so it is limited to C# 5 (no interpolated strings, no out var).
//
// Files are hashed while they are copied, so verifying and extracting the player's copy of
// the game is a single pass over the disc image.

using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace TT2Setup
{
    public class DiscEntry
    {
        public string Path;
        public long Offset;
        public long Size;
    }

    public static class Xdvdfs
    {
        const int Sector = 2048;
        static readonly byte[] Magic = Encoding.ASCII.GetBytes("MICROSOFT*XBOX*MEDIA");

        // Game partition offsets: XGD3, XGD2, XGD1, and plain XDVDFS images (extract-xiso output).
        static readonly long[] PartitionOffsets = { 0x2080000, 0xFD90000, 0x18300000, 0 };

        public static long FindPartition(Stream s)
        {
            byte[] buf = new byte[Magic.Length];
            foreach (long off in PartitionOffsets)
            {
                if (off + 32 * Sector + buf.Length > s.Length)
                    continue;
                s.Seek(off + 32 * Sector, SeekOrigin.Begin);
                if (s.Read(buf, 0, buf.Length) == buf.Length && Equal(buf, Magic))
                    return off;
            }
            return -1;
        }

        public static string LayoutName(long partition)
        {
            switch (partition)
            {
                case 0x2080000: return "XGD3";
                case 0xFD90000: return "XGD2";
                case 0x18300000: return "XGD1";
                case 0: return "XDVDFS";
                default: return "unknown";
            }
        }

        public static List<DiscEntry> List(string imagePath)
        {
            using (FileStream s = new FileStream(imagePath, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                long part = FindPartition(s);
                if (part < 0)
                    throw new InvalidDataException("Not an Xbox 360 disc image (no XDVDFS volume found).");

                byte[] head = new byte[8];
                s.Seek(part + 32 * Sector + Magic.Length, SeekOrigin.Begin);
                ReadExact(s, head, 8);
                uint rootSector = BitConverter.ToUInt32(head, 0);
                uint rootSize = BitConverter.ToUInt32(head, 4);

                List<DiscEntry> files = new List<DiscEntry>();
                ReadDirectory(s, part, rootSector, rootSize, "", files, 0);
                return files;
            }
        }

        static void ReadDirectory(Stream s, long part, uint sector, uint size, string prefix, List<DiscEntry> files, int depth)
        {
            if (depth > 32 || size == 0)
                return;
            byte[] table = new byte[size];
            s.Seek(part + (long)sector * Sector, SeekOrigin.Begin);
            ReadExact(s, table, (int)size);

            // Entries form a binary tree; walk it with an explicit stack.
            Stack<int> stack = new Stack<int>();
            HashSet<int> seen = new HashSet<int>();
            stack.Push(0);
            while (stack.Count > 0)
            {
                int o = stack.Pop() * 4;
                if (o + 14 > table.Length || !seen.Add(o))
                    continue;
                ushort left = BitConverter.ToUInt16(table, o);
                ushort right = BitConverter.ToUInt16(table, o + 2);
                if (left == 0xFFFF)
                    continue;
                uint start = BitConverter.ToUInt32(table, o + 4);
                uint length = BitConverter.ToUInt32(table, o + 8);
                byte attributes = table[o + 12];
                int nameLength = table[o + 13];
                if (o + 14 + nameLength > table.Length)
                    continue;
                string name = Encoding.GetEncoding(28591).GetString(table, o + 14, nameLength);
                string path = prefix.Length == 0 ? name : prefix + "/" + name;

                if ((attributes & 0x10) != 0)
                    ReadDirectory(s, part, start, length, path, files, depth + 1);
                else
                {
                    DiscEntry e = new DiscEntry();
                    e.Path = path;
                    e.Offset = part + (long)start * Sector;
                    e.Size = length;
                    files.Add(e);
                }

                if (left != 0) stack.Push(left);
                if (right != 0) stack.Push(right);
            }
        }

        // Copies `size` bytes at `offset` of `source` into `destination` and returns the SHA-1
        // (lowercase hex). `progress` receives the number of bytes copied since the last call.
        public static string CopyHashed(string source, long offset, long size, string destination, Action<long> progress)
        {
            string dir = System.IO.Path.GetDirectoryName(destination);
            if (!string.IsNullOrEmpty(dir))
                Directory.CreateDirectory(dir);

            using (FileStream input = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20))
            using (FileStream output = new FileStream(destination + ".partial", FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
            using (SHA1 sha = SHA1.Create())
            {
                input.Seek(offset, SeekOrigin.Begin);
                byte[] buffer = new byte[4 << 20];
                long left = size;
                while (left > 0)
                {
                    int want = (int)Math.Min(buffer.Length, left);
                    int got = input.Read(buffer, 0, want);
                    if (got <= 0)
                        throw new EndOfStreamException("Disc image is truncated.");
                    sha.TransformBlock(buffer, 0, got, null, 0);
                    output.Write(buffer, 0, got);
                    left -= got;
                    if (progress != null)
                        progress(got);
                }
                sha.TransformFinalBlock(buffer, 0, 0);
                output.Flush(true);
                output.Close();
                if (File.Exists(destination))
                    File.Delete(destination);
                File.Move(destination + ".partial", destination);
                return Hex(sha.Hash);
            }
        }

        public static string HashFile(string path, Action<long> progress)
        {
            using (FileStream input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20))
            using (SHA1 sha = SHA1.Create())
            {
                byte[] buffer = new byte[4 << 20];
                int got;
                while ((got = input.Read(buffer, 0, buffer.Length)) > 0)
                {
                    sha.TransformBlock(buffer, 0, got, null, 0);
                    if (progress != null)
                        progress(got);
                }
                sha.TransformFinalBlock(buffer, 0, 0);
                return Hex(sha.Hash);
            }
        }

        static string Hex(byte[] bytes)
        {
            StringBuilder sb = new StringBuilder(bytes.Length * 2);
            foreach (byte b in bytes)
                sb.Append(b.ToString("x2"));
            return sb.ToString();
        }

        static bool Equal(byte[] a, byte[] b)
        {
            if (a.Length != b.Length) return false;
            for (int i = 0; i < a.Length; i++)
                if (a[i] != b[i]) return false;
            return true;
        }

        static void ReadExact(Stream s, byte[] buf, int count)
        {
            int done = 0;
            while (done < count)
            {
                int got = s.Read(buf, done, count - done);
                if (got <= 0)
                    throw new EndOfStreamException("Disc image is truncated.");
                done += got;
            }
        }
    }
}
