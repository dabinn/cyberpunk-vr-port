using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Windows.Forms;

namespace CyberpunkVRPort.AutoInstaller
{
    internal sealed class MainForm : Form
    {
        private const string ConfigFileName = "CyberpunkVRPort-Auto-Installer.ini";
        private readonly string packageRoot = AppDomain.CurrentDomain.BaseDirectory;
        private readonly string configPath;
        private readonly IniDocument ini;
        private readonly ComboBox languageBox = new ComboBox();
        private readonly Label titleLabel = new Label();
        private readonly Label gamePathLabel = new Label();
        private readonly TextBox gamePathBox = new TextBox();
        private readonly Button browseButton = new Button();
        private readonly Button installButton = new Button();
        private readonly Button uninstallButton = new Button();
        private readonly Label statusLabel = new Label();
        private LanguagePack language;
        private string statusKey = "Ready";

        internal MainForm()
        {
            configPath = Path.Combine(packageRoot, ConfigFileName);
            ini = File.Exists(configPath) ? IniDocument.Load(configPath) : new IniDocument();
            var languages = Localization.Load(ini);
            language = Localization.ChooseDefault(languages);

            AutoScaleMode = AutoScaleMode.Dpi;
            Font = new Font("Segoe UI", 9F);
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(720, 324);
            MinimumSize = Size;
            MaximumSize = Size;
            MaximizeBox = false;
            FormBorderStyle = FormBorderStyle.FixedSingle;
            BackgroundImageLayout = ImageLayout.Stretch;
            using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(
                "CyberpunkVRPort.AutoInstaller.installer-background.png"))
            {
                if (stream != null)
                {
                    using (var image = Image.FromStream(stream)) BackgroundImage = new Bitmap(image);
                }
            }
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            titleLabel.AutoSize = true;
            titleLabel.Font = new Font("Segoe UI Semibold", 16F, FontStyle.Bold);
            titleLabel.ForeColor = Color.White;
            titleLabel.Anchor = AnchorStyles.Left;

            languageBox.DropDownStyle = ComboBoxStyle.DropDownList;
            languageBox.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            languageBox.Items.AddRange(languages.Cast<object>().ToArray());
            languageBox.SelectedItem = language;
            languageBox.SelectedIndexChanged += (_, __) =>
            {
                language = (LanguagePack)languageBox.SelectedItem;
                ApplyLanguage();
            };

            gamePathLabel.AutoSize = true;
            gamePathLabel.Anchor = AnchorStyles.Left;
            gamePathLabel.ForeColor = Color.White;
            gamePathBox.Dock = DockStyle.Fill;
            browseButton.AutoSize = true;
            StyleActionButton(browseButton, Color.FromArgb(55, 67, 102));
            browseButton.Click += Browse;
            installButton.AutoSize = true;
            installButton.MinimumSize = new Size(130, 38);
            StyleActionButton(installButton, Color.FromArgb(25, 103, 210));
            installButton.Click += (_, __) => RunOperation(true);
            uninstallButton.AutoSize = true;
            uninstallButton.MinimumSize = new Size(130, 38);
            StyleActionButton(uninstallButton, Color.FromArgb(55, 67, 102));
            uninstallButton.Click += (_, __) => RunOperation(false);
            statusLabel.AutoSize = true;
            statusLabel.ForeColor = Color.Gainsboro;
            statusLabel.Anchor = AnchorStyles.Left;

            var pathRow = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, AutoSize = true, BackColor = Color.Transparent };
            pathRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            pathRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            pathRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            pathRow.Controls.Add(gamePathLabel, 0, 0);
            pathRow.Controls.Add(gamePathBox, 1, 0);
            pathRow.Controls.Add(browseButton, 2, 0);

            var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.LeftToRight, AutoSize = true, BackColor = Color.Transparent };
            actions.Controls.Add(installButton);
            actions.Controls.Add(uninstallButton);

            var layout = new TableLayoutPanel { Dock = DockStyle.Fill, RowCount = 5, Padding = new Padding(22), BackColor = Color.Transparent };
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            layout.Controls.Add(titleLabel, 0, 0);
            layout.Controls.Add(languageBox, 0, 1);
            layout.Controls.Add(pathRow, 0, 2);
            layout.Controls.Add(actions, 0, 3);
            layout.Controls.Add(statusLabel, 0, 4);

            var contentPanel = new Panel { Dock = DockStyle.Fill, BackColor = Color.Transparent };
            contentPanel.Controls.Add(layout);
            var backgroundHost = new TableLayoutPanel
            {
                Dock = DockStyle.Fill,
                BackColor = Color.Transparent,
                Padding = new Padding(36, 28, 36, 28)
            };
            backgroundHost.Controls.Add(contentPanel, 0, 0);
            Controls.Add(backgroundHost);

            var detected = SteamLocator.FindGameRoots().FirstOrDefault();
            if (detected != null)
            {
                gamePathBox.Text = detected;
                statusKey = "Detected";
            }
            else
            {
                statusKey = "NotDetected";
            }
            ApplyLanguage();
        }

        private void ApplyLanguage()
        {
            Text = T("Title", "Cyberpunk VR Port Auto Installer");
            titleLabel.Text = Text;
            gamePathLabel.Text = T("GamePath", "Cyberpunk 2077 folder") + ":";
            browseButton.Text = T("Browse", "Browse...");
            installButton.Text = T("Install", "Install");
            uninstallButton.Text = T("Uninstall", "Uninstall");
            statusLabel.Text = T(statusKey, string.Empty);
        }

        private void Browse(object sender, EventArgs eventArgs)
        {
            using (var dialog = new FolderBrowserDialog())
            {
                dialog.Description = T("SelectGameFolder", "Select the Cyberpunk 2077 installation folder");
                if (Directory.Exists(gamePathBox.Text)) dialog.SelectedPath = gamePathBox.Text;
                if (dialog.ShowDialog(this) == DialogResult.OK) gamePathBox.Text = dialog.SelectedPath;
            }
        }

        private void RunOperation(bool install)
        {
            if (!File.Exists(configPath))
            {
                ShowError(T("MissingConfig", ConfigFileName + " is missing beside this program."));
                return;
            }
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

            try
            {
                var engine = new InstallerEngine(packageRoot, ini);
                if (install && !engine.PayloadExists)
                {
                    ShowError(T("MissingPayload", "The payload folder is missing beside this program."));
                    return;
                }
                var prompt = install ? T("ConfirmInstall", "Install into the selected folder?") : T("ConfirmUninstall", "Uninstall from the selected folder?");
                if (MessageBox.Show(this, prompt + Environment.NewLine + Environment.NewLine + Path.GetFullPath(gamePathBox.Text), Text,
                        MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;

                UseWaitCursor = true;
                var count = install ? engine.Install(gamePathBox.Text) : engine.Uninstall(gamePathBox.Text);
                statusLabel.ForeColor = Color.PaleGreen;
                statusLabel.Text = string.Format(install
                    ? T("InstallComplete", "Installation completed: {0} file(s) copied.")
                    : T("UninstallComplete", "Uninstall completed: {0} file(s) removed."), count);
                MessageBox.Show(this, statusLabel.Text, Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            catch (Exception exception)
            {
                ShowError(exception.Message);
            }
            finally
            {
                UseWaitCursor = false;
            }
        }

        private string T(string key, string fallback) => language.Get(key, fallback);

        private static void StyleActionButton(Button button, Color background)
        {
            button.FlatStyle = FlatStyle.Flat;
            button.FlatAppearance.BorderColor = Color.FromArgb(115, 155, 230);
            button.BackColor = background;
            button.ForeColor = Color.White;
            button.UseVisualStyleBackColor = false;
        }

        private void ShowError(string message)
        {
            statusLabel.ForeColor = Color.LightSalmon;
            statusLabel.Text = message;
            MessageBox.Show(this, message, T("OperationFailed", "Operation failed"), MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }
}
