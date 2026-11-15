using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;
using Windows.UI.Core;

namespace XournalStudio.Views;

/// <summary>
/// An InkToolbar drives a single InkCanvas, but every page has its own. The toolbar targets one of
/// them and this class copies the toolbar's tool to all other pages' InkPresenters.
/// </summary>
public sealed class InkToolSync
{
    // Xournal++'s default palette.
    private static readonly Color[] PenColors =
    [
        Color.FromArgb(255, 0x00, 0x00, 0x00), // black
        Color.FromArgb(255, 0x33, 0x33, 0xcc), // blue
        Color.FromArgb(255, 0xff, 0x00, 0x00), // red
        Color.FromArgb(255, 0x00, 0x80, 0x00), // green
        Color.FromArgb(255, 0x80, 0x80, 0x80), // gray
        Color.FromArgb(255, 0x00, 0xc0, 0xff), // light blue
        Color.FromArgb(255, 0x00, 0xff, 0x00), // light green
        Color.FromArgb(255, 0xff, 0x00, 0xff), // magenta
        Color.FromArgb(255, 0xff, 0x80, 0x00), // orange
        Color.FromArgb(255, 0xff, 0xff, 0x00), // yellow
    ];

    private readonly InkToolbar toolbar;
    private readonly List<InkPresenter> presenters = new();

    public InkToolSync(InkToolbar toolbar, InkToolbarBallpointPenButton pen, InkToolbarHighlighterButton highlighter)
    {
        this.toolbar = toolbar;

        pen.Palette = PenColors.Select(c => (Brush)new SolidColorBrush(c)).ToList();
        pen.SelectedBrushIndex = 1;
        // Xournal's pen widths: fine 0.85, medium 1.41, thick 2.26.
        pen.MinStrokeWidth = 0.5;
        pen.MaxStrokeWidth = 6;
        pen.SelectedStrokeWidth = 1.41;
        highlighter.MinStrokeWidth = 2;
        highlighter.MaxStrokeWidth = 30;
        highlighter.SelectedStrokeWidth = 8.5;

        toolbar.ActiveToolChanged += (_, _) => ApplyToAll();
        toolbar.InkDrawingAttributesChanged += (_, _) => ApplyToAll();
    }

    public void SetPresenters(IEnumerable<(InkCanvas Canvas, InkPresenter Presenter)> pages)
    {
        presenters.Clear();
        InkCanvas? first = null;
        foreach (var (canvas, presenter) in pages)
        {
            first ??= canvas;
            // Pen draws (mouse too, for convenience); touch is left to the ScrollView for panning and zooming.
            presenter.InputDeviceTypes = CoreInputDeviceTypes.Pen | CoreInputDeviceTypes.Mouse;
            presenters.Add(presenter);
        }
        toolbar.TargetInkCanvas = first;
        ApplyToAll();
    }

    private void ApplyToAll()
    {
        bool erasing = toolbar.ActiveTool is InkToolbarEraserButton;
        var attributes = toolbar.InkDrawingAttributes;
        if (attributes != null)
        {
            attributes.FitToCurve = true;
        }
        foreach (var presenter in presenters)
        {
            presenter.InputProcessingConfiguration.Mode =
                erasing ? InkInputProcessingMode.Erasing : InkInputProcessingMode.Inking;
            if (attributes != null)
            {
                presenter.UpdateDefaultDrawingAttributes(attributes);
            }
        }
    }
}
