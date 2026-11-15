using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.Windows.Storage.Pickers;
using Windows.Foundation;
using XournalStudio.Model;
using XournalStudio.Services;
using XournalStudio.Views;

namespace XournalStudio;

public sealed partial class MainWindow : Window
{
    private readonly AppSettings settings = AppSettings.Load();
    private readonly InkUndoStack undo = new();
    private readonly InkToolSync tools;
    private readonly DispatcherQueueTimer viewTimer;

    private XoppDocument document = XoppDocument.CreateNew();
    private PdfBackground? pdf;
    private bool dirty;
    private bool fitWidthPending = true;
    private bool closeConfirmed;

    private IEnumerable<PageView> PageViews => PagesPanel.Children.OfType<PageView>();

    public MainWindow(string? fileToOpen)
    {
        InitializeComponent();
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "app.ico"));
        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.Maximize();
        }

        tools = new InkToolSync(Toolbar, PenButton, HighlighterButton);
        undo.Changed += (_, _) =>
        {
            UndoButton.IsEnabled = UndoMenuItem.IsEnabled = undo.CanUndo;
            RedoButton.IsEnabled = RedoMenuItem.IsEnabled = undo.CanRedo;
        };
        undo.Changed += (_, _) => SetDirty(true);

        viewTimer = DispatcherQueue.CreateTimer();
        viewTimer.Interval = TimeSpan.FromMilliseconds(120);
        viewTimer.IsRepeating = false;
        viewTimer.Tick += (_, _) => UpdateVisiblePages();

        Activated += OnActivated;
        AppWindow.Closing += OnClosing;

        string? file = fileToOpen ?? settings.LastFile;
        if (file != null && File.Exists(file))
        {
            _ = OpenAsync(file);
        }
        else
        {
            ShowDocument(XoppDocument.CreateNew(), null);
        }
    }

    // ---------------------------------------------------------------- document

    private async Task OpenAsync(string path)
    {
        try
        {
            if (Path.GetExtension(path).Equals(".pdf", StringComparison.OrdinalIgnoreCase))
            {
                // Annotations of "x.pdf" live in "x.xopp" next to it, created on the first save.
                string xopp = Path.ChangeExtension(path, ".xopp");
                if (File.Exists(xopp))
                {
                    await OpenAsync(xopp);
                    return;
                }
                var pdfBackground = await PdfBackground.LoadAsync(path);
                var doc = XoppDocument.FromPdf(path, pdfBackground.GetPageSizes());
                doc.FilePath = xopp;
                ShowDocument(doc, pdfBackground);
            }
            else
            {
                var doc = XoppDocument.Load(path);
                PdfBackground? pdfBackground = null;
                if (doc.PdfPath != null)
                {
                    try
                    {
                        pdfBackground = await PdfBackground.LoadAsync(doc.PdfPath);
                    }
                    catch (Exception e)
                    {
                        ShowMessage(InfoBarSeverity.Warning, "PDF-Hintergrund nicht gefunden", $"{doc.PdfPath}: {e.Message}");
                    }
                }
                ShowDocument(doc, pdfBackground);
            }
            settings.LastFile = path;
            settings.Save();
        }
        catch (Exception e)
        {
            ShowMessage(InfoBarSeverity.Error, "Datei konnte nicht geöffnet werden", $"{path}: {e.Message}");
        }
    }

    private void ShowDocument(XoppDocument doc, PdfBackground? pdfBackground)
    {
        document = doc;
        pdf = pdfBackground;
        undo.Clear();
        PagesPanel.Children.Clear();
        foreach (var page in doc.Pages)
        {
            PagesPanel.Children.Add(CreatePageView(page));
        }
        OnPagesChanged();
        SetDirty(false);
        fitWidthPending = true;
        Scroller.ScrollTo(0, 0, new ScrollingScrollOptions(ScrollingAnimationMode.Disabled));
        FitWidthWhenReady();
    }

    private PageView CreatePageView(XoppPage page)
    {
        var view = new PageView(page, pdf);
        undo.Attach(page, view.Ink);
        return view;
    }

    private void OnPagesChanged()
    {
        tools.SetPresenters(PageViews.Select(v => (v.InkCanvas, v.Ink)));
        viewTimer.Start();
    }

    private bool Save()
    {
        if (document.FilePath == null)
        {
            return false;
        }
        try
        {
            document.Save(document.FilePath);
            SetDirty(false);
            settings.LastFile = document.FilePath;
            settings.Save();
            return true;
        }
        catch (Exception e)
        {
            ShowMessage(InfoBarSeverity.Error, "Speichern fehlgeschlagen", $"{document.FilePath}: {e.Message}");
            return false;
        }
    }

    private async Task<bool> SaveAsAsync()
    {
        var picker = new FileSavePicker(AppWindow.Id)
        {
            SuggestedFileName = Path.GetFileNameWithoutExtension(document.FilePath ?? "Notizen"),
        };
        picker.FileTypeChoices.Add("Xournal++-Dokument", [".xopp"]);
        var result = await picker.PickSaveFileAsync();
        if (result == null)
        {
            return false;
        }
        document.FilePath = result.Path;
        return Save();
    }

    private void SetDirty(bool value)
    {
        dirty = value;
        string name = document.FilePath != null ? Path.GetFileName(document.FilePath) : "Unbenannt";
        Title = $"{(dirty ? "• " : "")}{name} – Xournal Studio";
    }

    // ---------------------------------------------------------------- view

    private void FitWidthWhenReady()
    {
        if (Scroller.ViewportWidth <= 0 || document.Pages.Count == 0)
        {
            return;
        }
        fitWidthPending = false;
        double pageWidth = document.Pages.Max(p => p.Width) + 32;
        float zoom = (float)Math.Clamp(Scroller.ViewportWidth / pageWidth, Scroller.MinZoomFactor, Scroller.MaxZoomFactor);
        Scroller.ZoomTo(zoom, System.Numerics.Vector2.Zero, new ScrollingZoomOptions(ScrollingAnimationMode.Disabled));

        // Test hook: start at page XOURNAL_STUDIO_TEST_PAGE (1-based).
        if (int.TryParse(Environment.GetEnvironmentVariable("XOURNAL_STUDIO_TEST_PAGE"), out int testPage) &&
            testPage >= 1 && testPage <= PagesPanel.Children.Count)
        {
            PagesPanel.UpdateLayout();
            double y = PagesPanel.Children[testPage - 1].ActualOffset.Y * zoom;
            Scroller.ScrollTo(0, y, new ScrollingScrollOptions(ScrollingAnimationMode.Disabled));
        }
    }

    private void Zoom(float factor)
    {
        var center = new System.Numerics.Vector2((float)Scroller.ViewportWidth / 2, (float)Scroller.ViewportHeight / 2);
        Scroller.ZoomTo((float)Math.Clamp(Scroller.ZoomFactor * factor, Scroller.MinZoomFactor, Scroller.MaxZoomFactor), center);
    }

    private void OnScrollerSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (fitWidthPending)
        {
            FitWidthWhenReady();
        }
        viewTimer.Start();
    }

    private void OnViewChanged(ScrollView sender, object args) => viewTimer.Start();

    /// <summary>Renders PDF backgrounds of pages near the viewport at the current zoom; frees the others.</summary>
    private void UpdateVisiblePages()
    {
        var viewport = new Rect(0, 0, Scroller.ViewportWidth, Scroller.ViewportHeight);
        var near = new Rect(0, -viewport.Height, viewport.Width, viewport.Height * 3);
        double scale = Scroller.ZoomFactor * (Content.XamlRoot?.RasterizationScale ?? 1.0);
        DebugLog.Write($"zoom={Scroller.ZoomFactor} offset={Scroller.HorizontalOffset},{Scroller.VerticalOffset} " +
            $"extent={Scroller.ExtentWidth}x{Scroller.ExtentHeight} viewport={Scroller.ViewportWidth}x{Scroller.ViewportHeight} " +
            $"panel={PagesPanel.ActualWidth}x{PagesPanel.ActualHeight}");
        foreach (var view in PageViews)
        {
            var bounds = view.TransformToVisual(Scroller).TransformBounds(new Rect(0, 0, view.Width, view.Height));
            view.UpdateBackground(Intersects(bounds, near), scale);
        }
    }

    /// <summary>The page in the middle of the viewport (or the last one above it).</summary>
    private int CurrentPageIndex()
    {
        double middle = Scroller.ViewportHeight / 2;
        int index = 0, i = 0;
        foreach (var view in PageViews)
        {
            var bounds = view.TransformToVisual(Scroller).TransformBounds(new Rect(0, 0, view.Width, view.Height));
            if (bounds.Top <= middle)
            {
                index = i;
            }
            i++;
        }
        return index;
    }

    private static bool Intersects(Rect a, Rect b) =>
        a.Left < b.Right && b.Left < a.Right && a.Top < b.Bottom && b.Top < a.Bottom;

    private void ShowMessage(InfoBarSeverity severity, string title, string message)
    {
        DebugLog.Write($"{severity}: {title}: {message}");
        Info.Severity = severity;
        Info.Title = title;
        Info.Message = message;
        Info.IsOpen = true;
    }

    // ---------------------------------------------------------------- window

    private void OnActivated(object sender, WindowActivatedEventArgs args)
    {
        // Like Xournal Studio: save when the window loses focus.
        if (args.WindowActivationState == WindowActivationState.Deactivated && dirty && document.FilePath != null)
        {
            Save();
        }
    }

    private async void OnClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (closeConfirmed || !dirty)
        {
            return;
        }
        if (document.FilePath != null)
        {
            if (Save())
            {
                return;
            }
            args.Cancel = true;
            return;
        }

        args.Cancel = true;
        var dialog = new ContentDialog
        {
            XamlRoot = Content.XamlRoot,
            Title = "Änderungen speichern?",
            Content = "Das Dokument wurde noch nicht gespeichert.",
            PrimaryButtonText = "Speichern",
            SecondaryButtonText = "Nicht speichern",
            CloseButtonText = "Abbrechen",
            DefaultButton = ContentDialogButton.Primary,
        };
        var choice = await dialog.ShowAsync();
        if (choice == ContentDialogResult.None || (choice == ContentDialogResult.Primary && !await SaveAsAsync()))
        {
            return;
        }
        closeConfirmed = true;
        Close();
    }

    // ---------------------------------------------------------------- commands

    private async void OnNew(object sender, RoutedEventArgs e)
    {
        if (await SaveBeforeReplacingAsync())
        {
            ShowDocument(XoppDocument.CreateNew(), null);
        }
    }

    private async void OnOpen(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker(AppWindow.Id);
        picker.FileTypeFilter.Add(".xopp");
        picker.FileTypeFilter.Add(".pdf");
        var result = await picker.PickSingleFileAsync();
        if (result != null && await SaveBeforeReplacingAsync())
        {
            await OpenAsync(result.Path);
        }
    }

    private async void OnSave(object sender, RoutedEventArgs e)
    {
        if (document.FilePath == null)
        {
            await SaveAsAsync();
        }
        else
        {
            Save();
        }
    }

    private async void OnSaveAs(object sender, RoutedEventArgs e) => await SaveAsAsync();

    /// <summary>Saves a modified document before another one replaces it; false = keep it open.</summary>
    private async Task<bool> SaveBeforeReplacingAsync()
    {
        if (!dirty)
        {
            return true;
        }
        return document.FilePath != null ? Save() : await SaveAsAsync();
    }

    private void OnUndo(object sender, RoutedEventArgs e) => undo.Undo();

    private void OnRedo(object sender, RoutedEventArgs e) => undo.Redo();

    private void OnInsertPage(object sender, RoutedEventArgs e)
    {
        int index = document.Pages.Count == 0 ? -1 : CurrentPageIndex();
        var current = index >= 0 ? document.Pages[index] : null;
        var page = XoppPage.CreateGraph(current?.Width ?? XoppDocument.A4Width, current?.Height ?? XoppDocument.A4Height);
        document.Pages.Insert(index + 1, page);
        PagesPanel.Children.Insert(index + 1, CreatePageView(page));
        OnPagesChanged();
        SetDirty(true);
    }

    private async void OnDeletePage(object sender, RoutedEventArgs e)
    {
        if (document.Pages.Count <= 1)
        {
            return;
        }
        int index = CurrentPageIndex();
        var page = document.Pages[index];
        if (page.Strokes.Count > 0)
        {
            var dialog = new ContentDialog
            {
                XamlRoot = Content.XamlRoot,
                Title = $"Seite {index + 1} löschen?",
                Content = "Die Seite enthält Notizen.",
                PrimaryButtonText = "Löschen",
                CloseButtonText = "Abbrechen",
                DefaultButton = ContentDialogButton.Close,
            };
            if (await dialog.ShowAsync() != ContentDialogResult.Primary)
            {
                return;
            }
        }
        document.Pages.RemoveAt(index);
        PagesPanel.Children.RemoveAt(index);
        undo.Forget(page);
        OnPagesChanged();
        SetDirty(true);
    }

    private void OnFitWidth(object sender, RoutedEventArgs e) => FitWidthWhenReady();

    private void OnZoomIn(object sender, RoutedEventArgs e) => Zoom(1.25f);

    private void OnZoomOut(object sender, RoutedEventArgs e) => Zoom(0.8f);
}
