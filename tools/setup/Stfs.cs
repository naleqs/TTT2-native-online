// Xbox 360 content package (STFS: CON/LIVE/PIRS) reader used by setup.ps1 for the player's own
// DLC and title updates. Compiled at setup time with PowerShell's Add-Type, so it is limited to
// C# 5 (no interpolated strings, no out var).
//
// Extraction checks every 4 KB data block against the SHA-1 in its hash table, so a damaged or
// altered package is refused. Block addressing follows Xenia's StfsContainerDevice.

using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace TT2Setup
{
    public class StfsFile
    {
        public string Path;   // inside the package, '/' separated
        public long Size;
        public int StartBlock;
        public int BlockCount;
    }

    public class StfsPackage : IDisposable
    {
        public const uint ContentSavedGame = 0x00000001;
        public const uint ContentMarketplace = 0x00000002;
        public const uint ContentTitleUpdate = 0x000B0000;

        public string Magic;          // "CON ", "LIVE" or "PIRS"
        public uint ContentType;
        public uint TitleId;
        public uint Version;
        public string DisplayName;
        public string TitleName;
        public List<StfsFile> Files = new List<StfsFile>();

        readonly FileStream stream;
        readonly long dataStart;
        readonly int blockShift;
        readonly int totalBlocks;
        // Level-0 hash tables (one per 170 data blocks), cached so reading stays sequential.
        readonly Dictionary<long, byte[]> tables = new Dictionary<long, byte[]>();

        public static bool IsPackage(string path)
        {
            try
            {
                using (FileStream s = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
                {
                    if (s.Length < 0x1000) return false;
                    byte[] m = new byte[4];
                    if (s.Read(m, 0, 4) != 4) return false;
                    string magic = Encoding.ASCII.GetString(m);
                    return magic == "CON " || magic == "LIVE" || magic == "PIRS";
                }
            }
            catch (IOException) { return false; }
        }

        public StfsPackage(string path)
        {
            stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16);
            byte[] h = new byte[0x1800];
            ReadAt(0, h, h.Length);
            Magic = Encoding.ASCII.GetString(h, 0, 4);
            if (Magic != "CON " && Magic != "LIVE" && Magic != "PIRS")
                throw new InvalidDataException("Not an Xbox 360 content package.");

            uint headerSize = BE32(h, 0x340);
            ContentType = BE32(h, 0x344);
            Version = BE32(h, 0x358);
            TitleId = BE32(h, 0x360);
            if (BE32(h, 0x3A9) != 0)
                throw new InvalidDataException("SVOD packages (installed games) are not supported.");
            DisplayName = Utf16Be(h, 0x411, 0x80);
            TitleName = Utf16Be(h, 0x1691, 0x80);

            // STFS volume descriptor at 0x379.
            byte flags = h[0x379 + 2];
            int tableBlocks = h[0x379 + 3] | (h[0x379 + 4] << 8);
            int tableBlock = h[0x379 + 5] | (h[0x379 + 6] << 8) | (h[0x379 + 7] << 16);
            totalBlocks = (int)BE32(h, 0x379 + 28);

            bool con = Magic == "CON ";
            if (((headerSize + 0xFFF) & 0xB000) == 0xB000 || (flags & 1) != 0)
                blockShift = con ? 1 : 0;
            else
                blockShift = con ? 0 : 1;
            dataStart = (headerSize + 0xFFF) & ~0xFFFL;

            ReadFileTable(tableBlock, tableBlocks);
        }

        public void Dispose() { stream.Dispose(); }

        long BlockOffset(long index)
        {
            long block = index, b = 170;
            for (int i = 0; i < 3; i++)
            {
                block += ((index + b) / b) << blockShift;
                if (index < b) break;
                b *= 170;
            }
            return dataStart + (block << 12);
        }

        // Reads data block `index`, checks it against its level-0 hash entry (both table copies
        // when the package keeps two) and returns the next block in the chain.
        int ReadBlock(int index, byte[] block, SHA1 sha)
        {
            if (index < 0 || index >= totalBlocks)
                throw new InvalidDataException("Damaged package: block " + index + " is out of range.");
            ReadAt(BlockOffset(index), block, 0x1000);
            byte[] digest = sha.ComputeHash(block, 0, 0x1000);
            long table = BlockOffset((index / 170) * 170) - (0x1000L << blockShift);
            byte[] tableData;
            if (!tables.TryGetValue(table, out tableData))
            {
                if (tables.Count > 64)
                    tables.Clear();
                tableData = new byte[0x1000 << blockShift];
                ReadAt(table, tableData, tableData.Length);
                tables[table] = tableData;
            }
            for (int copy = 0; copy < (1 << blockShift); copy++)
            {
                int e = (copy << 12) + (index % 170) * 24;
                bool match = true;
                for (int i = 0; i < 20 && match; i++)
                    match = tableData[e + i] == digest[i];
                if (match)
                    return (tableData[e + 21] << 16) | (tableData[e + 22] << 8) | tableData[e + 23];
            }
            throw new InvalidDataException("Damaged or modified package: block " + index + " does not match its hash.");
        }

        void ReadFileTable(int first, int count)
        {
            List<string> dirNames = new List<string>();
            List<int> parents = new List<int>();
            List<bool> isDir = new List<bool>();
            List<StfsFile> entries = new List<StfsFile>();
            byte[] block = new byte[0x1000];
            using (SHA1 sha = SHA1.Create())
            {
                int b = first;
                for (int n = 0; n < count; n++)
                {
                    int next = ReadBlock(b, block, sha);
                    for (int o = 0; o < 0x1000; o += 0x40)
                    {
                        if (block[o] == 0) continue;
                        int nameLength = block[o + 0x28] & 0x3F;
                        StfsFile f = new StfsFile();
                        f.Path = Encoding.ASCII.GetString(block, o, nameLength);
                        f.BlockCount = block[o + 0x29] | (block[o + 0x2A] << 8) | (block[o + 0x2B] << 16);
                        f.StartBlock = block[o + 0x2F] | (block[o + 0x30] << 8) | (block[o + 0x31] << 16);
                        parents.Add((short)((block[o + 0x32] << 8) | block[o + 0x33]));
                        f.Size = BE32(block, o + 0x34);
                        isDir.Add((block[o + 0x28] & 0x80) != 0);
                        entries.Add(f);
                    }
                    b = next;
                }
            }
            // Full paths: the path indicator is the index of the parent directory entry (-1: root).
            for (int i = 0; i < entries.Count; i++)
            {
                if (isDir[i]) continue;
                string p = entries[i].Path;
                int parent = parents[i];
                for (int depth = 0; parent >= 0 && parent < entries.Count && depth < 32; depth++)
                {
                    p = entries[parent].Path + "/" + p;
                    parent = parents[parent];
                }
                if (p.Contains("..") || p.IndexOfAny(new char[] { ':', '\\' }) >= 0)
                    throw new InvalidDataException("Package contains an unsafe file name: " + p);
                entries[i].Path = p;
                Files.Add(entries[i]);
            }
        }

        // Extracts every file under `directory`, verifying each block. `progress` receives the
        // number of bytes written since the last call.
        public void ExtractAll(string directory, Action<long> progress)
        {
            long pending = 0;
            byte[] block = new byte[0x1000];
            using (SHA1 sha = SHA1.Create())
            {
                foreach (StfsFile f in Files)
                {
                    string dest = System.IO.Path.Combine(directory, f.Path.Replace('/', '\\'));
                    Directory.CreateDirectory(System.IO.Path.GetDirectoryName(dest));
                    using (FileStream output = new FileStream(dest + ".partial", FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
                    {
                        long left = f.Size;
                        int b = f.StartBlock;
                        while (left > 0)
                        {
                            int next = ReadBlock(b, block, sha);
                            int n = (int)Math.Min(0x1000, left);
                            output.Write(block, 0, n);
                            left -= n;
                            b = next;
                            pending += n;
                            if (progress != null && pending >= (16 << 20)) { progress(pending); pending = 0; }
                        }
                        output.Flush(true);
                    }
                    if (File.Exists(dest)) File.Delete(dest);
                    File.Move(dest + ".partial", dest);
                }
            }
            if (progress != null && pending > 0) progress(pending);
        }

        // Checks every block of every file without writing anything.
        public void VerifyAll(Action<long> progress)
        {
            long pending = 0;
            byte[] block = new byte[0x1000];
            using (SHA1 sha = SHA1.Create())
            {
                foreach (StfsFile f in Files)
                {
                    long left = f.Size;
                    int b = f.StartBlock;
                    while (left > 0)
                    {
                        int next = ReadBlock(b, block, sha);
                        int n = (int)Math.Min(0x1000, left);
                        left -= n;
                        b = next;
                        pending += n;
                        if (progress != null && pending >= (16 << 20)) { progress(pending); pending = 0; }
                    }
                }
            }
        }

        public long TotalSize()
        {
            long total = 0;
            foreach (StfsFile f in Files) total += f.Size;
            return total;
        }

        void ReadAt(long offset, byte[] buffer, int count)
        {
            stream.Seek(offset, SeekOrigin.Begin);
            int done = 0;
            while (done < count)
            {
                int got = stream.Read(buffer, done, count - done);
                if (got <= 0) throw new EndOfStreamException("Package is truncated.");
                done += got;
            }
        }

        static uint BE32(byte[] b, int o)
        {
            return ((uint)b[o] << 24) | ((uint)b[o + 1] << 16) | ((uint)b[o + 2] << 8) | b[o + 3];
        }

        static string Utf16Be(byte[] b, int o, int bytes)
        {
            string s = Encoding.BigEndianUnicode.GetString(b, o, bytes);
            int z = s.IndexOf('\0');
            return z >= 0 ? s.Substring(0, z) : s;
        }
    }
}
