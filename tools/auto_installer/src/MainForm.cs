using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace CyberpunkVRPort.AutoInstaller
{
    internal sealed class MainForm : Form
    {
        private const float FieldLabelColumnWidth = 125F;
        private const float FieldActionColumnWidth = 120F;
        private readonly IniDocument ini;
        private readonly bool devMode;
        private readonly bool resumeInstall;
        private readonly bool resumeUninstall;
        private readonly long? resumeReleaseId;
        private readonly string resumeGameRoot;
        private readonly string resumeLanguageCode;
        private readonly string resumeForkOwner;
        private readonly string resumeForkRepository;
        private readonly string executableDirectory = AppDomain.CurrentDomain.BaseDirectory;
        private readonly string payloadDirectory;
        private readonly string installerVersion;
        private readonly InstallerEngine engine;
        private readonly GitHubReleaseClient installerClient;
        private GitHubReleaseClient releaseClient;
        private readonly List<DevForkDefinition> forks;
        private readonly ComboBox languageBox = new ComboBox();
        private readonly ComboBox sourceBox = new ComboBox();
        private readonly ComboBox forkBox = new ComboBox();
        private readonly ComboBox releaseBox = new ComboBox();
        private readonly Label titleLabel = new Label();
        private readonly Label sourceLabel = new Label();
        private readonly Label forkLabel = new Label();
        private readonly Label versionLabel = new Label();
        private readonly Label localPackageLabel = new Label();
        private readonly Label gamePathLabel = new Label();
        private readonly TextBox gamePathBox = new TextBox();
        private readonly ComboBox localPathBox = new ComboBox();
        private readonly Button browseButton = new Button();
        private readonly Button refreshButton = new Button();
        private readonly Button localButton = new Button();
        private readonly Button installButton = new Button();
        private readonly Button uninstallButton = new Button();
        private readonly Label statusLabel = new Label();
        private readonly LinkLabel installerStatusLabel = new LinkLabel();
        private readonly Label installationStatusLabel = new Label();
        private readonly Label vrportIniPrefixLabel = new Label();
        private readonly LinkLabel vrportIniStatusLabel = new LinkLabel();
        private readonly FlowLayoutPanel vrportIniStatusRow = new FlowLayoutPanel();
        private readonly LinkLabel releaseNotesLink = new LinkLabel();
        private readonly TableLayoutPanel sourceRow = new TableLayoutPanel();
        private readonly TableLayoutPanel forkRow = new TableLayoutPanel();
        private readonly TableLayoutPanel releaseRow = new TableLayoutPanel();
        private readonly TableLayoutPanel localRow = new TableLayoutPanel();
        private List<GitHubRelease> releases = new List<GitHubRelease>();
        private GitHubRelease latestInstallerRelease;
        private LanguagePack language;
        private string statusKey = "Ready";
        private string statusArgument;
        private string installerStatusKey;
        private bool loadingReleases;
        private bool resumeSelectionPending;
        private DevForkDefinition activeFork;
        private bool controlsInitialized;
        private bool busy;

        internal MainForm(IniDocument ini, bool devMode, string[] launchArguments)
        {
            this.ini = ini;
            this.devMode = devMode;
            var arguments = launchArguments ?? Array.Empty<string>();
            long releaseId = 0;
            resumeInstall = !devMode && arguments.Length >= 3 &&
                arguments[0].Equals("--resume-install", StringComparison.OrdinalIgnoreCase) &&
                long.TryParse(arguments[1], out releaseId);
            resumeUninstall = !devMode && arguments.Length >= 2 &&
                arguments[0].Equals("--resume-uninstall", StringComparison.OrdinalIgnoreCase);
            resumeReleaseId = resumeInstall ? (long?)releaseId : null;
            resumeSelectionPending = resumeInstall;
            resumeGameRoot = resumeInstall ? arguments[2] : resumeUninstall ? arguments[1] : null;
            resumeLanguageCode = resumeInstall && arguments.Length >= 4
                ? arguments[3]
                : resumeUninstall && arguments.Length >= 3 ? arguments[2] : null;
            resumeForkOwner = resumeInstall && arguments.Length >= 6 ? arguments[4] : null;
            resumeForkRepository = resumeInstall && arguments.Length >= 6 ? arguments[5] : null;
            installerStatusKey = devMode ? "InstallerStatusDev" : "InstallerStatusChecking";
            payloadDirectory = ini.Get("Installer", "PayloadDirectory", "Cyberpunk 2077");
            var assemblyVersion = Assembly.GetExecutingAssembly().GetName().Version;
            installerVersion = assemblyVersion.Major + "." + assemblyVersion.Minor;
            engine = new InstallerEngine(ini);
            installerClient = new GitHubReleaseClient(ini);
            releaseClient = installerClient;
            forks = LoadForks(ini, devMode, executableDirectory);
            var languages = Localization.Load(ini);
            language = languages.FirstOrDefault(pack => pack.Code.Equals(resumeLanguageCode, StringComparison.OrdinalIgnoreCase))
                ?? Localization.ChooseDefault(languages);

            AutoScaleMode = AutoScaleMode.Dpi;
            Font = new Font("Segoe UI", 9F);
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(720, devMode ? 476 : 430);
            MinimumSize = Size;
            MaximumSize = Size;
            MaximizeBox = false;
            FormBorderStyle = FormBorderStyle.FixedSingle;
            BackgroundImageLayout = ImageLayout.Stretch;
            using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(
                "CyberpunkVRPort.AutoInstaller.installer-background.png"))
            {
                if (stream != null)
                    using (var image = Image.FromStream(stream)) BackgroundImage = new Bitmap(image);
            }
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            titleLabel.AutoSize = true;
            titleLabel.Font = new Font("Segoe UI Semibold", 16F, FontStyle.Bold);
            titleLabel.ForeColor = Color.White;
            languageBox.DropDownStyle = ComboBoxStyle.DropDownList;
            languageBox.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            languageBox.Width = 140;
            languageBox.Items.AddRange(languages.Cast<object>().ToArray());
            languageBox.SelectedItem = language;
            languageBox.SelectedIndexChanged += (_, __) =>
            {
                language = (LanguagePack)languageBox.SelectedItem;
                ApplyLanguage();
            };

            sourceBox.DropDownStyle = ComboBoxStyle.DropDownList;
            sourceBox.SelectedIndexChanged += async (_, __) => await SourceChangedAsync();
            forkBox.DropDownStyle = ComboBoxStyle.DropDownList;
            forkBox.SelectedIndexChanged += async (_, __) => await ForkChangedAsync();
            releaseBox.DropDownStyle = ComboBoxStyle.DropDownList;
            releaseBox.SelectedIndexChanged += (_, __) => UpdateReleaseNotesLink();
            localPathBox.DropDownStyle = ComboBoxStyle.DropDownList;
            localPathBox.Dock = DockStyle.Fill;
            gamePathBox.Dock = DockStyle.Fill;
            gamePathBox.TextChanged += (_, __) => RefreshInstallationStatus();

            ConfigureButton(browseButton, Color.FromArgb(55, 67, 102));
            ConfigureButton(refreshButton, Color.FromArgb(55, 67, 102));
            ConfigureButton(localButton, Color.FromArgb(55, 67, 102));
            ConfigureButton(installButton, Color.FromArgb(25, 103, 210));
            ConfigureButton(uninstallButton, Color.FromArgb(55, 67, 102));
            installButton.MinimumSize = new Size(130, 38);
            uninstallButton.MinimumSize = new Size(130, 38);
            browseButton.Click += BrowseGame;
            refreshButton.Click += async (_, __) =>
            {
                try { await LoadReleasesAsync(true, true); }
                finally { RefreshInstallationStatus(); }
            };
            localButton.Click += (_, __) =>
            {
                RefreshLocalPackages();
                RefreshInstallationStatus();
            };
            installButton.Click += async (_, __) => await RunOperationAsync(true);
            uninstallButton.Click += async (_, __) => await RunOperationAsync(false);
            statusLabel.AutoSize = true;
            statusLabel.ForeColor = Color.Gainsboro;
            statusLabel.Anchor = AnchorStyles.Left | AnchorStyles.Bottom;
            installerStatusLabel.AutoSize = true;
            installerStatusLabel.ForeColor = Color.Gainsboro;
            installerStatusLabel.Anchor = AnchorStyles.Right | AnchorStyles.Bottom;
            installerStatusLabel.TextAlign = ContentAlignment.MiddleRight;
            installerStatusLabel.LinkBehavior = LinkBehavior.NeverUnderline;
            installerStatusLabel.DisabledLinkColor = Color.Gainsboro;
            installerStatusLabel.LinkClicked += async (_, __) => await UpdateInstallerNowAsync();
            installationStatusLabel.AutoSize = true;
            installationStatusLabel.ForeColor = Color.Gainsboro;
            vrportIniPrefixLabel.AutoSize = true;
            vrportIniPrefixLabel.ForeColor = Color.Gainsboro;
            vrportIniPrefixLabel.Margin = Padding.Empty;
            vrportIniStatusLabel.AutoSize = true;
            vrportIniStatusLabel.ForeColor = Color.Gainsboro;
            vrportIniStatusLabel.LinkColor = Color.Gainsboro;
            vrportIniStatusLabel.ActiveLinkColor = Color.White;
            vrportIniStatusLabel.VisitedLinkColor = Color.Gainsboro;
            vrportIniStatusLabel.LinkBehavior = LinkBehavior.NeverUnderline;
            vrportIniStatusLabel.DisabledLinkColor = Color.Gainsboro;
            vrportIniStatusLabel.Margin = new Padding(3, 0, 0, 0);
            vrportIniStatusLabel.LinkClicked += ToggleForceCreateVrportIni;
            vrportIniStatusRow.AutoSize = true;
            vrportIniStatusRow.BackColor = Color.Transparent;
            vrportIniStatusRow.FlowDirection = FlowDirection.LeftToRight;
            vrportIniStatusRow.WrapContents = false;
            vrportIniStatusRow.Margin = new Padding(15, 3, 3, 3);
            vrportIniStatusRow.Controls.Add(vrportIniPrefixLabel);
            vrportIniStatusRow.Controls.Add(vrportIniStatusLabel);
            releaseNotesLink.AutoSize = true;
            releaseNotesLink.LinkColor = Color.LightSkyBlue;
            releaseNotesLink.ActiveLinkColor = Color.White;
            releaseNotesLink.VisitedLinkColor = Color.LightSkyBlue;
            releaseNotesLink.LinkClicked += OpenReleaseNotes;

            BuildRows();
            var actions = new FlowLayoutPanel
            {
                Dock = DockStyle.Fill, FlowDirection = FlowDirection.LeftToRight,
                AutoSize = true, BackColor = Color.Transparent
            };
            actions.Controls.Add(installButton);
            actions.Controls.Add(uninstallButton);

            var layout = new TableLayoutPanel
            {
                Dock = DockStyle.Fill, RowCount = 10, Padding = new Padding(22), BackColor = Color.Transparent
            };
            for (var row = 0; row < 9; row++) layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            layout.Controls.Add(BuildTitleRow(), 0, 0);
            layout.Controls.Add(BuildGamePathRow(), 0, 1);
            layout.Controls.Add(sourceRow, 0, 2);
            layout.Controls.Add(forkRow, 0, 3);
            layout.Controls.Add(releaseRow, 0, 4);
            layout.Controls.Add(localRow, 0, 5);
            layout.Controls.Add(actions, 0, 6);
            layout.Controls.Add(installationStatusLabel, 0, 7);
            layout.Controls.Add(vrportIniStatusRow, 0, 8);
            layout.Controls.Add(BuildFooterRow(), 0, 9);

            var backgroundHost = new TableLayoutPanel
            {
                Dock = DockStyle.Fill, BackColor = Color.Transparent, Padding = new Padding(36, 24, 36, 24)
            };
            backgroundHost.Controls.Add(layout, 0, 0);
            Controls.Add(backgroundHost);

            var detected = SteamLocator.FindGameRoots().FirstOrDefault();
            if (detected != null)
            {
                gamePathBox.Text = detected;
                statusKey = "Detected";
            }
            else statusKey = "NotDetected";

            ApplyLanguage();
            Shown += async (_, __) =>
            {
                if (!devMode)
                {
                    SelectForkClient();
                    await LoadReleasesAsync(false, true);
                }
                await ResumeOperationAsync();
            };
            FormClosed += (_, __) =>
            {
                if (!ReferenceEquals(releaseClient, installerClient)) releaseClient.Dispose();
                installerClient.Dispose();
            };
        }

        private void BuildRows()
        {
            ConfigureRow(sourceRow, sourceLabel, sourceBox, null);
            ConfigureRow(forkRow, forkLabel, forkBox, releaseNotesLink);
            ConfigureRow(releaseRow, versionLabel, releaseBox, refreshButton);
            ConfigureRow(localRow, localPackageLabel, localPathBox, localButton);
            sourceRow.Visible = devMode;
            forkRow.Visible = false;
            localRow.Visible = devMode;
            forkBox.Items.AddRange(forks.Cast<object>().ToArray());
            var resumeFork = forks.FirstOrDefault(fork =>
                fork.Owner.Equals(resumeForkOwner, StringComparison.OrdinalIgnoreCase) &&
                fork.Repository.Equals(resumeForkRepository, StringComparison.OrdinalIgnoreCase));
            forkBox.SelectedItem = resumeFork ?? forks.First();
            if (devMode)
            {
                sourceBox.Items.AddRange(new object[] { "Online", "Local" });
                sourceBox.SelectedIndex = 1;
                RefreshLocalPackages();
            }
            controlsInitialized = true;
        }

        private TableLayoutPanel BuildGamePathRow()
        {
            var row = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, AutoSize = true, BackColor = Color.Transparent };
            ConfigureRow(row, gamePathLabel, gamePathBox, browseButton);
            return row;
        }

        private TableLayoutPanel BuildTitleRow()
        {
            var row = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, AutoSize = true, BackColor = Color.Transparent };
            row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            row.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            row.Controls.Add(titleLabel, 0, 0);
            row.Controls.Add(languageBox, 1, 0);
            return row;
        }

        private TableLayoutPanel BuildFooterRow()
        {
            var row = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, AutoSize = true, BackColor = Color.Transparent };
            row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            row.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            row.Controls.Add(statusLabel, 0, 0);
            row.Controls.Add(installerStatusLabel, 1, 0);
            return row;
        }

        private static void ConfigureRow(TableLayoutPanel row, Label label, Control value, Control action)
        {
            row.Dock = DockStyle.Fill;
            row.ColumnCount = 3;
            row.AutoSize = true;
            row.BackColor = Color.Transparent;
            row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, FieldLabelColumnWidth));
            row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, FieldActionColumnWidth));
            label.AutoSize = false;
            label.Dock = DockStyle.Fill;
            label.TextAlign = ContentAlignment.MiddleLeft;
            label.ForeColor = Color.White;
            label.Anchor = AnchorStyles.Left;
            value.Dock = DockStyle.Fill;
            row.Controls.Add(label, 0, 0);
            row.Controls.Add(value, 1, 0);
            if (action != null)
            {
                action.AutoSize = action is LinkLabel;
                if (action is LinkLabel) action.Anchor = AnchorStyles.Left;
                else
                {
                    action.Height = 30;
                    action.Anchor = AnchorStyles.Left | AnchorStyles.Right;
                }
                row.Controls.Add(action, 2, 0);
            }
        }

        private void ApplyLanguage()
        {
            Text = T("Title", "Cyberpunk VR Port Auto Installer");
            titleLabel.Text = Text;
            sourceLabel.Text = T("Source", "Source") + ":";
            forkLabel.Text = T("Fork", "Fork") + ":";
            versionLabel.Text = T("Version", "Version") + ":";
            localPackageLabel.Text = T("LocalPackage", "Local package") + ":";
            gamePathLabel.Text = T("GamePath", "Cyberpunk 2077 folder") + ":";
            browseButton.Text = T("Browse", "Browse...");
            refreshButton.Text = T("Refresh", "Refresh");
            localButton.Text = T("Refresh", "Refresh");
            installButton.Text = T("Install", "Install");
            uninstallButton.Text = T("Cleanup", "Cleanup");
            releaseNotesLink.Text = T("ReleaseNotes", "Notes");
            if (devMode)
            {
                var selected = sourceBox.SelectedIndex;
                sourceBox.Items.Clear();
                sourceBox.Items.Add(T("Online", "Online"));
                sourceBox.Items.Add(T("Local", "Local (Dev Mode)"));
                sourceBox.SelectedIndex = selected < 0 ? 1 : selected;
            }
            SetStatus(statusKey, statusArgument, false);
            SetInstallerStatus(installerStatusKey);
            UpdateSourceRows();
            RefreshInstallationStatus();
            UpdateReleaseNotesLink();
        }

        private async Task SourceChangedAsync()
        {
            if (!controlsInitialized) return;
            UpdateSourceRows();
            if (IsOnline)
            {
                SelectForkClient();
                if (releases.Count == 0) await LoadReleasesAsync(false, false);
            }
            else if (!IsOnline)
            {
                RefreshLocalPackages();
                SetStatus("DevMode", null, false);
            }
        }

        private async Task ForkChangedAsync()
        {
            if (!controlsInitialized || !IsOnline) return;
            SelectForkClient();
            releases.Clear();
            releaseBox.Items.Clear();
            await LoadReleasesAsync(false, false);
        }

        private void UpdateSourceRows()
        {
            var online = IsOnline;
            forkRow.Visible = online;
            releaseRow.Visible = online;
            localRow.Visible = devMode && !online;
            UpdateReleaseNotesLink();
        }

        private bool IsOnline => !devMode || sourceBox.SelectedIndex == 0;

        private async Task LoadReleasesAsync(bool refresh, bool refreshInstaller)
        {
            if (loadingReleases) return;
            loadingReleases = true;
            SetBusy(true);
            SetStatus("LoadingReleases", null, false);
            try
            {
                var publishedReleases = await releaseClient.GetReleasesAsync();
                releases = publishedReleases.Where(release => release.ZipAsset != null).ToList();
                if (devMode)
                {
                    UpdateInstallerStatus(null);
                }
                else if (ShouldRefreshInstaller(refreshInstaller,
                    ReferenceEquals(releaseClient, installerClient)))
                {
                    var installerReleases = ReferenceEquals(releaseClient, installerClient)
                        ? publishedReleases
                        : await installerClient.GetReleasesAsync();
                    latestInstallerRelease = installerReleases.FirstOrDefault(release => release.InstallerAsset != null);
                    UpdateInstallerStatus(latestInstallerRelease);
                }
                releaseBox.Items.Clear();
                releaseBox.Items.AddRange(releases.Cast<object>().ToArray());
                var preferred = resumeSelectionPending && resumeReleaseId.HasValue
                    ? releases.FirstOrDefault(release => release.Id == resumeReleaseId.Value)
                    : null;
                preferred = preferred ?? releases.FirstOrDefault(release => !release.Prerelease) ?? releases.FirstOrDefault();
                if (preferred != null) releaseBox.SelectedItem = preferred;
                resumeSelectionPending = false;
                if (releases.Count == 0) SetStatus("NoReleases", null, true);
                else if (releaseClient.RateLimitRemaining.HasValue)
                    SetStatus("ReadyWithRateLimit", releaseClient.RateLimitRemaining.Value.ToString(), false);
                else SetStatus("Ready", null, false);
            }
            catch (Exception exception)
            {
                SetInstallerStatus(devMode ? "InstallerStatusDev" : "InstallerStatusUnavailable");
                SetStatus("ReleaseLoadFailed", exception.Message, true);
                if (refresh) ShowError(string.Format(T("ReleaseLoadFailed", "Could not load GitHub releases: {0}"), exception.Message));
            }
            finally
            {
                loadingReleases = false;
                SetBusy(false);
            }
        }

        private bool ShouldRefreshInstaller(bool refreshInstaller, bool selectedOwnRepository)
        {
            return !devMode && (refreshInstaller || selectedOwnRepository);
        }

        private async Task RunOperationAsync(bool install)
        {
            if (!SteamLocator.IsGameRoot(gamePathBox.Text))
            {
                ShowError(T("InvalidGame", "The selected folder is not Cyberpunk 2077."));
                return;
            }
            if (Process.GetProcessesByName("Cyberpunk2077").Length > 0)
            {
                ShowError(T("GameRunning", "Close Cyberpunk 2077 before continuing."));
                return;
            }
            if (install && IsOnline && !(releaseBox.SelectedItem is GitHubRelease))
            {
                ShowError(T("SelectRelease", "Select a release to install."));
                return;
            }
            if (install && !IsOnline && !(localPathBox.SelectedItem is LocalPackageItem))
            {
                ShowError(T("SelectPackage", "Select a local package folder or ZIP."));
                return;
            }

            if (ShouldRequireLatestInstaller(install) && !await EnsureLatestInstallerAsync(true)) return;

            var packageLabel = IsOnline
                ? Convert.ToString(releaseBox.SelectedItem)
                : Convert.ToString(localPathBox.SelectedItem);
            var prompt = install
                ? string.Format(T("ConfirmInstall", "Install {0} into the selected folder?"), packageLabel)
                : T("ConfirmUninstall", "Uninstall from the selected folder?");
            if (MessageBox.Show(this, prompt + Environment.NewLine + Environment.NewLine + Path.GetFullPath(gamePathBox.Text), Text,
                    MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;

            SetBusy(true);
            try
            {
                int count;
                if (!install)
                {
                    count = await Task.Run(() => engine.Uninstall(gamePathBox.Text));
                }
                else
                {
                    var packagePath = (localPathBox.SelectedItem as LocalPackageItem)?.FullPath;
                    if (IsOnline)
                    {
                        var release = (GitHubRelease)releaseBox.SelectedItem;
                        SetStatus("Downloading", release.ZipAsset.Name, false);
                        packagePath = await releaseClient.DownloadToCacheAsync(release, release.ZipAsset);
                        SetStatus("Verifying", null, false);
                    }
                    using (var package = Directory.Exists(packagePath)
                        ? PackageSource.OpenFolder(packagePath, payloadDirectory)
                        : PackageSource.OpenZip(packagePath, payloadDirectory))
                    {
                        var selectedRelease = releaseBox.SelectedItem as GitHubRelease;
                        var source = IsOnline ? activeFork.Owner : "Local";
                        var version = IsOnline
                            ? selectedRelease.ShortName
                            : ((LocalPackageItem)localPathBox.SelectedItem).DisplayName;
                        count = await Task.Run(() => engine.Install(package.PayloadRoot, package.DirectRootLayout,
                            gamePathBox.Text, source, version,
                            key => BeginInvoke((Action)(() => SetStatus(key, null, false)))));
                    }
                }
                statusLabel.ForeColor = Color.PaleGreen;
                statusLabel.Text = string.Format(install
                    ? T("InstallComplete", "Installation completed: {0} file(s) copied.")
                    : T("UninstallComplete", "Uninstall completed: {0} file(s) removed."), count);
                MessageBox.Show(this, statusLabel.Text, Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
                RefreshInstallationStatus();
            }
            catch (Exception exception)
            {
                ShowError(exception.GetBaseException().Message);
            }
            finally { SetBusy(false); }
        }

        private bool ShouldRequireLatestInstaller(bool install)
        {
            return install && !devMode;
        }

        private async Task<bool> EnsureLatestInstallerAsync(bool install)
        {
            SetBusy(true);
            try
            {
                var latest = latestInstallerRelease;
                if (latest == null) throw new InvalidDataException(T("NoReleases", "No compatible GitHub release was found."));
                if (installerStatusKey == "InstallerStatusCurrent") return true;
                if (installerStatusKey != "InstallerStatusUpdateAvailable")
                    throw new InvalidDataException(T("InstallerStatusUnavailable", "Installer status unavailable"));

                var selected = install ? releaseBox.SelectedItem as GitHubRelease : null;
                if (install && selected == null)
                {
                    ShowError(T("SelectRelease", "Select a release to install."));
                    return false;
                }
                var prompt = install
                    ? string.Format(T("InstallerUpdatePromptInstall",
                        "A newer Auto Installer is required. The app will update and restart, then continue installing {0}."), selected)
                    : T("InstallerUpdatePromptUninstall",
                        "A newer Auto Installer is required. The app will update and restart, then continue uninstalling.");
                if (MessageBox.Show(this, prompt, Text, MessageBoxButtons.OKCancel, MessageBoxIcon.Information) != DialogResult.OK)
                    return false;

                SetStatus("UpdatingInstaller", null, false);
                SetInstallerStatus("InstallerStatusUpdating");
                var gameRoot = Path.GetFullPath(gamePathBox.Text);
                var resumeArguments = install
                    ? new[]
                    {
                        "--resume-install", selected.Id.ToString(), gameRoot, language.Code,
                        activeFork.Owner, activeFork.Repository
                    }
                    : new[] { "--resume-uninstall", gameRoot, language.Code };
                await SelfUpdater.StartUpdateAsync(installerClient, latest, resumeArguments);
                Application.Exit();
                return false;
            }
            catch (Exception exception)
            {
                ShowError(exception.GetBaseException().Message);
                return false;
            }
            finally { SetBusy(false); }
        }

        private async Task ResumeOperationAsync()
        {
            if (devMode) return;
            if (resumeInstall)
            {
                var selected = releases.FirstOrDefault(release => release.Id == resumeReleaseId.Value);
                if (selected == null)
                {
                    ShowError(T("ResumeReleaseMissing", "The previously selected release is no longer available."));
                    return;
                }
                releaseBox.SelectedItem = selected;
                gamePathBox.Text = resumeGameRoot;
                await RunOperationAsync(true);
            }
            else if (resumeUninstall)
            {
                gamePathBox.Text = resumeGameRoot;
                await RunOperationAsync(false);
            }
        }

        private void RefreshLocalPackages()
        {
            var previousPath = (localPathBox.SelectedItem as LocalPackageItem)?.FullPath;
            var zipPattern = ini.Get("Installer", "ReleaseZipPattern", "^CyberpunkVR-.*\\.zip$");
            var packages = FindLocalPackages(executableDirectory, zipPattern);
            localPathBox.Items.Clear();
            localPathBox.Items.AddRange(packages.Cast<object>().ToArray());
            var previous = packages.FirstOrDefault(package => package.FullPath.Equals(
                previousPath, StringComparison.OrdinalIgnoreCase));
            if (previous != null) localPathBox.SelectedItem = previous;
            else if (packages.Count > 0) localPathBox.SelectedIndex = 0;
        }

        private static List<LocalPackageItem> FindLocalPackages(string directory, string zipPattern)
        {
            var pattern = new System.Text.RegularExpressions.Regex(zipPattern,
                System.Text.RegularExpressions.RegexOptions.IgnoreCase);
            var packages = new List<LocalPackageItem>();
            packages.AddRange(Directory.EnumerateDirectories(directory)
                .Where(path =>
                {
                    var name = Path.GetFileName(path.TrimEnd(
                        Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
                    return pattern.IsMatch(name) || pattern.IsMatch(name + ".zip");
                })
                .Select(path => new LocalPackageItem(path, Directory.GetLastWriteTimeUtc(path))));
            packages.AddRange(Directory.EnumerateFiles(directory, "*.zip")
                .Where(path => pattern.IsMatch(Path.GetFileName(path)))
                .Select(path => new LocalPackageItem(path, File.GetLastWriteTimeUtc(path))));
            return packages.OrderByDescending(package => package.LastWriteTimeUtc)
                .ThenBy(package => package.DisplayName, StringComparer.OrdinalIgnoreCase)
                .ToList();
        }

        private void BrowseGame(object sender, EventArgs eventArgs)
        {
            using (var dialog = new FolderBrowserDialog())
            {
                dialog.Description = T("SelectGameFolder", "Select the Cyberpunk 2077 installation folder");
                if (Directory.Exists(gamePathBox.Text)) dialog.SelectedPath = gamePathBox.Text;
                if (dialog.ShowDialog(this) == DialogResult.OK) gamePathBox.Text = dialog.SelectedPath;
            }
        }

        private void SetBusy(bool busy)
        {
            this.busy = busy;
            UseWaitCursor = busy;
            installButton.Enabled = !busy;
            uninstallButton.Enabled = !busy;
            refreshButton.Enabled = !busy;
            sourceBox.Enabled = !busy;
            forkBox.Enabled = !busy;
            localButton.Enabled = !busy;
            installerStatusLabel.Enabled = !busy && installerStatusKey == "InstallerStatusUpdateAvailable";
            RefreshVrportIniLinkState();
        }

        private void SelectForkClient()
        {
            if (!(forkBox.SelectedItem is DevForkDefinition selected) || ReferenceEquals(selected, activeFork)) return;
            if (!ReferenceEquals(releaseClient, installerClient)) releaseClient.Dispose();
            activeFork = selected;
            var ownOwner = ini.Get("Installer", "RepositoryOwner", "dabinn");
            var ownRepository = ini.Get("Installer", "RepositoryName", "cyberpunk-vr-port");
            releaseClient = selected.Owner.Equals(ownOwner, StringComparison.OrdinalIgnoreCase) &&
                selected.Repository.Equals(ownRepository, StringComparison.OrdinalIgnoreCase)
                    ? installerClient
                    : new GitHubReleaseClient(selected.Owner, selected.Repository, selected.ZipPattern,
                        ini.Get("Installer", "InstallerAsset", "CyberpunkVRPort-Auto-Installer.exe"));
        }

        private static List<DevForkDefinition> LoadForks(IniDocument ini, bool devMode, string executableDirectory)
        {
            var owner = ini.Get("Installer", "RepositoryOwner", "dabinn");
            var repository = ini.Get("Installer", "RepositoryName", "cyberpunk-vr-port");
            var forks = new List<DevForkDefinition>
            {
                new DevForkDefinition("Tofu Express", owner, repository,
                    ini.Get("Installer", "ReleaseZipPattern", "^CyberpunkVR-.*\\.zip$"))
            };
            AppendForks(forks, ini.GetSection("Folks"));
            if (!devMode) return forks;

            var path = Path.Combine(executableDirectory, "CyberpunkVRPort-Auto-Installer.dev.ini");
            if (!File.Exists(path)) return forks;
            try
            {
                var external = IniDocument.Load(path);
                AppendForks(forks, external.GetSection("DevForks"));
            }
            catch (Exception exception)
            {
                InstallerLog.Write("ERROR Could not load DevForks: " + exception.Message);
            }
            return forks;
        }

        private sealed class LocalPackageItem
        {
            internal string FullPath { get; private set; }
            internal string DisplayName { get; private set; }
            internal DateTime LastWriteTimeUtc { get; private set; }

            internal LocalPackageItem(string fullPath, DateTime lastWriteTimeUtc)
            {
                FullPath = Path.GetFullPath(fullPath);
                DisplayName = Path.GetFileName(FullPath.TrimEnd(
                    Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
                LastWriteTimeUtc = lastWriteTimeUtc;
            }

            public override string ToString()
            {
                return DisplayName;
            }
        }

        private static void AppendForks(List<DevForkDefinition> forks,
            IReadOnlyList<KeyValuePair<string, string>> entries)
        {
            foreach (var entry in entries)
            {
                try
                {
                    var parts = entry.Value.Split(new[] { '|' }, 2);
                    var repositoryParts = parts[0].Split(new[] { '/' }, 2);
                    if (repositoryParts.Length != 2 || string.IsNullOrWhiteSpace(entry.Key)) continue;
                    var forkOwner = repositoryParts[0].Trim();
                    var forkRepository = repositoryParts[1].Trim();
                    if (forkOwner.Length == 0 || forkRepository.Length == 0) continue;
                    if (forks.Any(fork => fork.Owner.Equals(forkOwner, StringComparison.OrdinalIgnoreCase) &&
                        fork.Repository.Equals(forkRepository, StringComparison.OrdinalIgnoreCase))) continue;
                    var zipPattern = parts.Length == 2 && !string.IsNullOrWhiteSpace(parts[1])
                        ? parts[1].Trim()
                        : ".*\\.zip$";
                    try { new System.Text.RegularExpressions.Regex(zipPattern); }
                    catch (ArgumentException)
                    {
                        InstallerLog.Write("ERROR Invalid fork ZIP regex for " + entry.Key);
                        continue;
                    }
                    forks.Add(new DevForkDefinition(entry.Key.Trim(), forkOwner, forkRepository,
                        zipPattern));
                }
                catch (Exception exception)
                {
                    InstallerLog.Write("ERROR Could not load fork " + entry.Key + ": " + exception.Message);
                }
            }
        }

        private void SetStatus(string key, string argument, bool error)
        {
            statusKey = key;
            statusArgument = argument;
            var format = T(key, key);
            statusLabel.Text = argument == null ? format : string.Format(format, argument);
            statusLabel.ForeColor = error ? Color.LightSalmon : Color.Gainsboro;
            InstallerLog.Write((error ? "ERROR " : string.Empty) + statusLabel.Text);
        }

        private void UpdateInstallerStatus(GitHubRelease latest)
        {
            if (devMode)
            {
                SetInstallerStatus("InstallerStatusDev");
                return;
            }
            try
            {
                SetInstallerStatus(SelfUpdater.IsUpdateRequired(latest)
                    ? "InstallerStatusUpdateAvailable"
                    : "InstallerStatusCurrent");
            }
            catch
            {
                SetInstallerStatus("InstallerStatusUnavailable");
            }
        }

        private void SetInstallerStatus(string key)
        {
            installerStatusKey = key;
            installerStatusLabel.Text = "[v" + installerVersion + "] " + T(key, key);
            installerStatusLabel.Enabled = key == "InstallerStatusUpdateAvailable";
            installerStatusLabel.LinkColor = key == "InstallerStatusUpdateAvailable"
                ? Color.LightSkyBlue
                : Color.Gainsboro;
            installerStatusLabel.ForeColor = key == "InstallerStatusCurrent"
                ? Color.PaleGreen
                : key == "InstallerStatusUpdateAvailable" || key == "InstallerStatusUpdating"
                    ? Color.Khaki
                    : key == "InstallerStatusUnavailable"
                        ? Color.LightSalmon
                        : Color.Gainsboro;
        }

        private async Task UpdateInstallerNowAsync()
        {
            if (installerStatusKey != "InstallerStatusUpdateAvailable") return;
            var prompt = T("InstallerUpdatePromptNow",
                "The Auto Installer will update and restart.");
            if (MessageBox.Show(this, prompt, Text, MessageBoxButtons.OKCancel,
                    MessageBoxIcon.Information) != DialogResult.OK) return;

            SetBusy(true);
            try
            {
                if (latestInstallerRelease == null)
                    throw new InvalidDataException(T("InstallerStatusUnavailable", "Installer status unavailable"));
                SetStatus("UpdatingInstaller", null, false);
                SetInstallerStatus("InstallerStatusUpdating");
                await SelfUpdater.StartUpdateAsync(installerClient, latestInstallerRelease);
                Application.Exit();
            }
            catch (Exception exception)
            {
                SetInstallerStatus("InstallerStatusUnavailable");
                ShowError(exception.GetBaseException().Message);
            }
            finally { SetBusy(false); }
        }

        private void RefreshInstallationStatus()
        {
            var status = engine.GetInstallationStatus(gamePathBox.Text);
            uninstallButton.Text = status.HasInstallerState
                ? T("Uninstall", "Uninstall")
                : T("Cleanup", "Cleanup");
            installationStatusLabel.Text = !status.Installed
                ? T("InstalledNotYet", "Installed: Not yet")
                : status.ExternalInstall
                    ? T("InstalledExternal", "Installed: Yes (external install)")
                : string.IsNullOrWhiteSpace(status.Source) || string.IsNullOrWhiteSpace(status.Version)
                    ? T("InstalledYes", "Installed: Yes")
                    : string.Format(T("InstalledNamed", "Installed: {0}/{1}"), status.Source, status.Version);
            installationStatusLabel.ForeColor = status.Installed ? Color.PaleGreen : Color.Gainsboro;
            vrportIniPrefixLabel.Text = T("VrportIniPrefix", "└─ vrport.ini:");
            vrportIniStatusLabel.Text = status.VrportIniExists
                ? T("VrportIniFound", "Found")
                : T("VrportIniNotFound", "Not present");
            vrportIniStatusLabel.ForeColor = status.VrportIniExists ? Color.PaleGreen : Color.Gainsboro;
            RefreshVrportIniLinkState(status.VrportIniExists);
        }

        private void ToggleForceCreateVrportIni(object sender, LinkLabelLinkClickedEventArgs eventArgs)
        {
            var warning = T("VrportIniForceWarning",
                "Danger! vrport.ini will be forcibly created now. Use this only if you know what you are doing.");
            if (MessageBox.Show(this, warning, Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
                return;
            try { engine.ForceCreateVrportIni(gamePathBox.Text); }
            catch (Exception exception) { ShowError(exception.GetBaseException().Message); }
            finally { RefreshInstallationStatus(); }
        }

        private void RefreshVrportIniLinkState(bool? exists = null)
        {
            var isPresent = exists ?? engine.GetInstallationStatus(gamePathBox.Text).VrportIniExists;
            vrportIniStatusLabel.Enabled = !busy && !isPresent;
            vrportIniStatusLabel.LinkColor = Color.Gainsboro;
            vrportIniStatusLabel.ActiveLinkColor = Color.White;
            vrportIniStatusLabel.DisabledLinkColor = isPresent ? Color.PaleGreen : Color.Gainsboro;
        }

        private void UpdateReleaseNotesLink()
        {
            var release = releaseBox.SelectedItem as GitHubRelease;
            releaseNotesLink.Visible = IsOnline && release != null && release.HasReleaseNotes &&
                IsValidReleaseUrl(release.HtmlUrl);
        }

        private void OpenReleaseNotes(object sender, LinkLabelLinkClickedEventArgs eventArgs)
        {
            var release = releaseBox.SelectedItem as GitHubRelease;
            if (release == null || !IsValidReleaseUrl(release.HtmlUrl)) return;
            try
            {
                Process.Start(new ProcessStartInfo(release.HtmlUrl) { UseShellExecute = true });
                releaseNotesLink.LinkVisited = true;
            }
            catch (Exception exception)
            {
                ShowError(exception.Message);
            }
        }

        private static bool IsValidReleaseUrl(string value)
        {
            Uri uri;
            return Uri.TryCreate(value, UriKind.Absolute, out uri) &&
                (uri.Scheme == Uri.UriSchemeHttps || uri.Scheme == Uri.UriSchemeHttp);
        }

        private string T(string key, string fallback) => language.Get(key, fallback);

        private static void ConfigureButton(Button button, Color background)
        {
            button.AutoSize = true;
            button.FlatStyle = FlatStyle.Flat;
            button.FlatAppearance.BorderColor = Color.FromArgb(115, 155, 230);
            button.BackColor = background;
            button.ForeColor = Color.White;
            button.UseVisualStyleBackColor = false;
        }

        private void ShowError(string message)
        {
            InstallerLog.Write("ERROR " + message);
            statusLabel.ForeColor = Color.LightSalmon;
            statusLabel.Text = message;
            MessageBox.Show(this, message, T("OperationFailed", "Operation failed"), MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }
}
