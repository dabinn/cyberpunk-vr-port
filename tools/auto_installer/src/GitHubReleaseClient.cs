using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Web.Script.Serialization;

namespace CyberpunkVRPort.AutoInstaller
{
    internal sealed class ReleaseAsset
    {
        internal string Name { get; set; }
        internal long Size { get; set; }
        internal string DownloadUrl { get; set; }
        internal string Digest { get; set; }
    }

    internal sealed class GitHubRelease
    {
        internal long Id { get; set; }
        internal string TagName { get; set; }
        internal string Name { get; set; }
        internal string HtmlUrl { get; set; }
        internal bool HasReleaseNotes { get; set; }
        internal bool Prerelease { get; set; }
        internal DateTime PublishedAt { get; set; }
        internal ReleaseAsset ZipAsset { get; set; }
        internal ReleaseAsset InstallerAsset { get; set; }

        public override string ToString()
        {
            return Prerelease ? ShortName + " (pre-release)" : ShortName;
        }

        internal string ShortName => string.IsNullOrWhiteSpace(Name) ? TagName : Name;
    }

    internal sealed class DevForkDefinition
    {
        internal string DisplayName { get; }
        internal string Owner { get; }
        internal string Repository { get; }
        internal string ZipPattern { get; }

        internal DevForkDefinition(string displayName, string owner, string repository, string zipPattern)
        {
            DisplayName = displayName;
            Owner = owner;
            Repository = repository;
            ZipPattern = zipPattern;
        }

        public override string ToString() => DisplayName;
    }

    internal sealed class GitHubReleaseClient : IDisposable
    {
        private readonly HttpClient http = new HttpClient();
        private readonly string owner;
        private readonly string repository;
        private readonly Regex zipPattern;
        private readonly string installerAssetName;
        internal int? RateLimitRemaining { get; private set; }
        internal DateTimeOffset? RateLimitReset { get; private set; }

        internal GitHubReleaseClient(IniDocument ini)
            : this(
                ini.Get("Installer", "RepositoryOwner", "dabinn"),
                ini.Get("Installer", "RepositoryName", "cyberpunk-vr-port"),
                ini.Get("Installer", "ReleaseZipPattern", "^CyberpunkVR-.*\\.zip$"),
                ini.Get("Installer", "InstallerAsset", "CyberpunkVRPort-Auto-Installer.exe"))
        {
        }

        internal GitHubReleaseClient(string owner, string repository, string releaseZipPattern, string installerAssetName)
        {
            this.owner = owner;
            this.repository = repository;
            zipPattern = new Regex(releaseZipPattern, RegexOptions.IgnoreCase);
            this.installerAssetName = installerAssetName;
            var version = typeof(GitHubReleaseClient).Assembly.GetName().Version;
            http.DefaultRequestHeaders.UserAgent.ParseAdd(
                "CyberpunkVRPort-Auto-Installer/" + version.Major + "." + version.Minor);
            http.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
            http.Timeout = TimeSpan.FromMinutes(10);
        }

        internal async Task<List<GitHubRelease>> GetReleasesAsync()
        {
            var url = string.Format("https://api.github.com/repos/{0}/{1}/releases?per_page=100", owner, repository);
            string json;
            using (var response = await http.GetAsync(url).ConfigureAwait(false))
            {
                response.EnsureSuccessStatusCode();
                IEnumerable<string> headerValues;
                int remaining;
                long reset;
                if (response.Headers.TryGetValues("X-RateLimit-Remaining", out headerValues) &&
                    int.TryParse(headerValues.FirstOrDefault(), out remaining)) RateLimitRemaining = remaining;
                if (response.Headers.TryGetValues("X-RateLimit-Reset", out headerValues) &&
                    long.TryParse(headerValues.FirstOrDefault(), out reset)) RateLimitReset = DateTimeOffset.FromUnixTimeSeconds(reset);
                json = await response.Content.ReadAsStringAsync().ConfigureAwait(false);
            }
            var values = new JavaScriptSerializer { MaxJsonLength = int.MaxValue }.DeserializeObject(json) as object[];
            var releases = new List<GitHubRelease>();
            if (values == null) return releases;
            foreach (var value in values)
            {
                var item = value as Dictionary<string, object>;
                if (item == null || GetBool(item, "draft")) continue;
                var assets = ReadAssets(item);
                var zip = assets.FirstOrDefault(asset => zipPattern.IsMatch(asset.Name ?? string.Empty));
                var installer = assets.FirstOrDefault(asset =>
                    string.Equals(asset.Name, installerAssetName, StringComparison.OrdinalIgnoreCase));
                if (zip == null && installer == null) continue;
                var published = GetString(item, "published_at");
                DateTime publishedAt;
                DateTime.TryParse(published, out publishedAt);
                releases.Add(new GitHubRelease
                {
                    Id = GetLong(item, "id"),
                    TagName = GetString(item, "tag_name"),
                    Name = GetString(item, "name"),
                    HtmlUrl = GetString(item, "html_url"),
                    HasReleaseNotes = !string.IsNullOrWhiteSpace(GetString(item, "body")),
                    Prerelease = GetBool(item, "prerelease"),
                    PublishedAt = publishedAt,
                    ZipAsset = zip,
                    InstallerAsset = installer
                });
            }
            return releases.OrderByDescending(release => release.PublishedAt).ToList();
        }

        internal async Task<string> DownloadToCacheAsync(GitHubRelease release, ReleaseAsset asset)
        {
            if (asset == null) throw new InvalidDataException("The required release asset is missing.");
            var directory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "CyberpunkVRPort", "AutoInstaller", "cache", release.Id.ToString());
            Directory.CreateDirectory(directory);
            var safeName = Path.GetFileName(asset.Name);
            if (string.IsNullOrWhiteSpace(safeName) || !string.Equals(safeName, asset.Name, StringComparison.Ordinal))
                throw new InvalidDataException("Unsafe release asset name: " + asset.Name);
            var destination = Path.Combine(directory, safeName);
            if (File.Exists(destination) && VerifyAsset(destination, asset)) return destination;

            var partial = destination + ".download";
            if (File.Exists(partial)) File.Delete(partial);
            using (var response = await http.GetAsync(asset.DownloadUrl, HttpCompletionOption.ResponseHeadersRead).ConfigureAwait(false))
            {
                response.EnsureSuccessStatusCode();
                using (var input = await response.Content.ReadAsStreamAsync().ConfigureAwait(false))
                using (var output = new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None))
                    await input.CopyToAsync(output).ConfigureAwait(false);
            }
            if (!VerifyAsset(partial, asset))
            {
                File.Delete(partial);
                throw new InvalidDataException("Downloaded asset failed size or SHA-256 verification: " + asset.Name);
            }
            if (File.Exists(destination)) File.Delete(destination);
            File.Move(partial, destination);
            return destination;
        }

        internal static bool VerifyAsset(string path, ReleaseAsset asset)
        {
            var file = new FileInfo(path);
            if (!file.Exists || (asset.Size > 0 && file.Length != asset.Size)) return false;
            var expected = NormalizeDigest(asset.Digest);
            if (string.IsNullOrEmpty(expected)) return false;
            return string.Equals(ComputeSha256(path), expected, StringComparison.OrdinalIgnoreCase);
        }

        internal static string ComputeSha256(string path)
        {
            using (var sha = SHA256.Create())
            using (var stream = File.OpenRead(path))
                return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", string.Empty).ToLowerInvariant();
        }

        private static string NormalizeDigest(string digest)
        {
            if (string.IsNullOrWhiteSpace(digest)) return null;
            var colon = digest.IndexOf(':');
            return (colon >= 0 ? digest.Substring(colon + 1) : digest).Trim();
        }

        private static List<ReleaseAsset> ReadAssets(Dictionary<string, object> item)
        {
            object raw;
            var result = new List<ReleaseAsset>();
            var values = item.TryGetValue("assets", out raw) ? raw as object[] : null;
            if (values == null) return result;
            foreach (var value in values)
            {
                var asset = value as Dictionary<string, object>;
                if (asset == null) continue;
                result.Add(new ReleaseAsset
                {
                    Name = GetString(asset, "name"),
                    Size = GetLong(asset, "size"),
                    DownloadUrl = GetString(asset, "browser_download_url"),
                    Digest = GetString(asset, "digest")
                });
            }
            return result;
        }

        private static string GetString(Dictionary<string, object> item, string key)
        {
            object value;
            return item.TryGetValue(key, out value) && value != null ? Convert.ToString(value) : string.Empty;
        }

        private static bool GetBool(Dictionary<string, object> item, string key)
        {
            object value;
            return item.TryGetValue(key, out value) && value != null && Convert.ToBoolean(value);
        }

        private static long GetLong(Dictionary<string, object> item, string key)
        {
            object value;
            return item.TryGetValue(key, out value) && value != null ? Convert.ToInt64(value) : 0;
        }

        public void Dispose() => http.Dispose();
    }
}
