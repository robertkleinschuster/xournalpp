using InkPresenter = Microsoft.UI.Xaml.Controls.InkPresenter;
using InkStrokeContainer = Microsoft.UI.Xaml.Controls.InkStrokeContainer;
using Windows.UI.Input.Inking;
using XournalStudio.Model;

namespace XournalStudio.Services;

/// <summary>
/// Undo/redo for ink: records strokes drawn or erased through the pages' InkPresenters
/// (the WinUI InkCanvas has no undo of its own).
/// </summary>
public sealed class InkUndoStack
{
    private sealed class Entry(XoppPage page, bool added, List<InkStroke> strokes)
    {
        public XoppPage Page { get; } = page;
        public bool Added { get; } = added;
        public List<InkStroke> Strokes { get; set; } = strokes;
    }

    private readonly Stack<Entry> undo = new();
    private readonly Stack<Entry> redo = new();

    public event EventHandler? Changed;

    public bool CanUndo => undo.Count > 0;
    public bool CanRedo => redo.Count > 0;

    public void Attach(XoppPage page, InkPresenter presenter)
    {
        presenter.StrokesCollected += (_, e) => Push(new Entry(page, true, e.Strokes.ToList()));
        presenter.StrokesErased += (_, e) => Push(new Entry(page, false, e.Strokes.ToList()));
    }

    public void Clear()
    {
        undo.Clear();
        redo.Clear();
        Changed?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>Forgets the history of a deleted page.</summary>
    public void Forget(XoppPage page)
    {
        Filter(undo, page);
        Filter(redo, page);
        Changed?.Invoke(this, EventArgs.Empty);
    }

    public void Undo() => Move(undo, redo);

    public void Redo() => Move(redo, undo);

    private void Push(Entry entry)
    {
        undo.Push(entry);
        redo.Clear();
        Changed?.Invoke(this, EventArgs.Empty);
    }

    private void Move(Stack<Entry> from, Stack<Entry> to)
    {
        if (from.Count == 0)
        {
            return;
        }

        var entry = from.Pop();
        bool remove = from == undo ? entry.Added : !entry.Added;
        if (remove)
        {
            Remove(entry.Page.Ink!, entry.Strokes);
        }
        else
        {
            entry.Strokes = Restore(entry.Page, entry.Strokes);
        }
        to.Push(entry);
        Changed?.Invoke(this, EventArgs.Empty);
    }

    private static void Remove(InkStrokeContainer ink, List<InkStroke> strokes)
    {
        foreach (var s in ink.GetStrokes())
        {
            s.Selected = false;
        }
        foreach (var s in strokes)
        {
            s.Selected = true;
        }
        ink.DeleteSelected();
    }

    // A removed stroke cannot be added again; its copy takes its place (also in the file).
    private static List<InkStroke> Restore(XoppPage page, List<InkStroke> strokes)
    {
        var copies = new List<InkStroke>(strokes.Count);
        foreach (var s in strokes)
        {
            var copy = s.Clone();
            page.Ink!.AddStroke(copy);
            page.TransferStrokeIdentity(s.Id, copy.Id);
            copies.Add(copy);
        }
        return copies;
    }

    private static void Filter(Stack<Entry> stack, XoppPage page)
    {
        var kept = stack.Where(e => e.Page != page).Reverse().ToList();
        stack.Clear();
        foreach (var e in kept)
        {
            stack.Push(e);
        }
    }
}
