#pragma once

// xphone-os — Reader (R2a). EPUB reading on the CrossPoint engine port in
// src/reader: paginate-once section.bin caches, replay one Page per turn.
//
// State machine (see ReaderScene.cpp header comment for the full flow):
//   Opening  -> "Opening book..." frame, then deferred Epub::load + resume
//   Indexing -> "Indexing chapter..." frame, then deferred section build
//   Reading  -> deserialize + blit the current page each render; CONFIRM
//               ("SIZE") cycles 12/14/16 pt, keeping the position by ratio
//   BookList -> 2x2 cover-thumbnail grid over SD /books (+ root) *.epub
//   Error    -> message + BACK to the book list
//
// Blocking engine work (zip inflate + expat parse + DP line breaking, cover
// decode — up to seconds) NEVER runs inside onEnter()/render(): render()
// composes the progress frame, and the work runs on a later handleInput()
// tick once the flush worker has put that frame on glass. The book grid's
// metadata + cover thumbnails load the same way, one tile per quiet tick.

#include <cstdint>
#include <memory>
#include <string>

#include "../Scene.h"
#include "../reader/BookTextRenderer.h"
#include "../reader/Bookmarks.h"
#include "../reader/FbpBook.h"
#include "../reader/ReaderSettings.h"

namespace reader {
class Epub;
class Section;
class TextMeasure;
class FbpBook;
}  // namespace reader

class ReaderScene : public Scene {
 public:
  // Out-of-line (defaulted in the .cpp): the unique_ptr members hold types
  // that are only forward-declared here, and both the constructor and the
  // destructor of the enclosing class must see them complete.
  ReaderScene();
  ~ReaderScene();

  void onEnter() override;
  void onExit() override;
  // The next onEnter reopens the book without painting "Opening book...":
  // the page is still on glass (sync in place, 2026-09-05). The first
  // paint is the page itself, a differential refresh of the same pixels.
  void resumeQuietly() { _quietReopen = true; }
  // Bench dev console ("where" v2): sub-state + selection one-liner —
  // dead-reckoning from "scene=reader" alone opened a real book once.
  void debugWhere(char* out, size_t n) const;
  // Bench: print the live shelf order (index, meta state, name) so order
  // instability is measurable instead of anecdotal (audit 2026-08-18, I2).
  void debugShelfDump() const;
  void debugLineCids() const;  // v7 tail proof
  void acceptGoto(const char* key, uint32_t cid);  // X1 (called via AppScenes)
  const char* const* softKeys() const override;
  uint8_t longPressSlots() const override;
  void handleInput(Input& in) override;
  void render(Gfx& gfx) override;

 private:
  enum class State : uint8_t { Opening, Indexing, Reading, BookList, Error };
  // Deferred blocking work, run from handleInput() once the announcing frame
  // is on glass (armed by render(), gated on the flush worker being idle).
  // GridMeta (like PrefetchNext) is silent and yields to pending input.
  enum class Work : uint8_t { None, OpenBook, BuildSection, PrefetchNext, GridMeta };

  static constexpr uint16_t kLastPageSentinel = 0xFFFF;  // "prev chapter, last page"
  // In-RAM WINDOW of the shelf, not a library cap (0.6 workstream B): the
  // scan counts every book on the card and stores only the window around
  // the current scroll; crossing the window edge rescans. BSS stays fixed
  // no matter how many books the card holds.
  static constexpr int kMaxBooks = 32;
  // Per-tile lazy-load progress for the cover grid (workGridMeta). One work
  // unit takes a tile from Unknown to one of the three terminal states.
  enum class TileMeta : uint8_t {
    Unknown = 0,  // not probed yet — placeholder tile, grid work pending
    NotOpened,    // load+build failed — the epub is unreadable/corrupt
    NoCover,      // title/author loaded; no usable cover (text-only tile)
    Cover,        // title/author loaded; thumbPath/thumbW/thumbH valid
  };
  struct BookEntry {
    char path[160];           // full SD path incl. one /books subdir level; display name = basename(path)
    char title[64];           // from book.bin (basename fallback)
    char author[48];
    char thumbPath[64];       // cover_<w>x<h>.bin path (meta == Cover only)
    uint16_t thumbW, thumbH;  // actual thumb dims (aspect-fit, can undershoot)
    bool focusEdition;        // compiled with focus reading (bookc --focus)
    TileMeta meta;
    uint32_t lastReadDay;     // yyyymmdd from ReadingStats, 0 = never read
    uint16_t readMinutes;     // total minutes in this book (sort tiebreak)
    uint8_t progressPct;      // from the .pos sidecar, 0 = none/unknown
    uint32_t sizeBytes;       // captured free during the directory walk; the
                              // shelf index is keyed on (name, size)
  };

  // Shelf index (2026-09-08). Drawing the shelf used to open every compiled
  // package to read its title, author and cover flag — 1.8 s for 23 books on
  // the X4, growing with the library. The answer is CrossPoint's: never
  // re-parse a book to draw a list. One small file on the card holds a row
  // per package, and a row still matching its file's name and size is used
  // as-is. Only a new or changed book is opened, once.
  bool loadShelfIndex();   // fills entries it recognises; true if the file read
  void saveShelfIndex();   // rewrites the file from the entries now in hand

  // Deferred work.
  void runWork();
  void workOpenBook();
  void workBuildSection();
  void workPrefetchNext();
  void workGridMeta();

  // Engine plumbing.
  void ensureSectionOrIndex();  // load section cache or schedule Indexing
  void applyPendingPage();
  void loadProgress();
  void saveProgress();
  void maybeArmPrefetch();
  void failWith(const char* msg);

  // Book list (2x2 cover grid; windowed over an unbounded library).
  void scanBooks(int windowOffset = 0);
  void scanDir(const char* dir, int depth);
  // flowe-os#26: when the window is the whole library, finished books (last
  // page reached) are moved to the TAIL of _books and shown behind one "Done"
  // tile at the end of the shelf. _totalBooks then counts the CURRENT VIEW
  // (live books + the Done tile, or the done books). Windowed libraries
  // (> kMaxBooks) skip the split — the phone is the browsing surface there.
  bool shelfSplit() const { return _doneCount > 0 || _doneView; }
  bool isDoneTile(int absIdx) const { return !_doneView && _doneCount > 0 && absIdx == _liveCount; }
  bool inWindow(int absIdx) const {
    if (shelfSplit()) return absIdx >= 0 && absIdx < _totalBooks && !isDoneTile(absIdx);
    return absIdx >= _windowOffset && absIdx < _windowOffset + _bookCount;
  }
  BookEntry& entryAt(int absIdx) { return _books[(_doneView ? _liveCount : 0) + absIdx - _windowOffset]; }
  void applyShelfView();
  void renderDoneTile(Gfx& gfx, int visibleIndex);
  void enterBookList();
  bool shelfQuietBuildPending() const;  // R5: a visible EPUB tile needs the quiet-heap build
  void openSelectedBook();
  void moveSelection(int delta);
  XpRect listRect() const;
  // True when the open book has genuine chapters (fbp always; epub only when
  // it declared a TOC). Drives "Chapters" vs "Sections" wording — see #42.
  bool hasRealToc() const;
  XpRect tileRect(int visibleIndex) const;

  // Input/render helpers.
  void pageTurn(bool forward);
  void cycleFontSize();
  // 0.7 reader chrome v2: full-page MENU (the book's home) with two
  // strip sub-views that keep the page visible for live preview.
  // setFontSize is the retargetable core of cycleFontSize; sizeStep is
  // the strip's live UP/DOWN; noteTurnPace feeds time-left estimates.
  void setFontSize(int fontId);
  void sizeStep(int dir);
  void noteTurnPace();
  void handleMenuInput(Input& in);
  void menuSelect();
  void chapterCountAndSel(int* count, int* selOut);
  void chapterJump(int idx);
  float bookProgress();
  int estimateMinutesLeft(int pagesLeft) const;
  bool paceKnown() const { return _avgTurnMs != 0; }
  // Reader quality (2026-09-04): chapter-aware progress. Pages after this
  // one in the current chapter, -1 when unknown (no package or no TOC).
  // Cached per page: the answer costs two SD searches.
  int chapterPagesLeft(int* pagesInChapter);
  uint16_t _chapCachePage = 0xFFFF;
  int _chapCacheLeft = -1, _chapCacheTotal = 0;
  // Footer mode in the reading chrome's corner: 0 off, 1 page x of y,
  // 2 percent, 3 time left in this chapter (default; the most praised stat).
  uint8_t _footerMode = 3;
  void renderMenuPage(Gfx& gfx);
  void renderMenuBody(Gfx& gfx, int y);
  int renderFocusChip(Gfx& gfx, int y);
  void renderSizeStrip(Gfx& gfx);
  void renderChapters(Gfx& gfx);
  void renderGoToStrip(Gfx& gfx);
  void renderBookmarks(Gfx& gfx);
  void renderStatsBook(Gfx& gfx);
  void renderStatsLife(Gfx& gfx);
  void renderStatsResetConfirm(Gfx& gfx);
  // Bookmarks: current place -> a mark; jumping restores it.
  bool applyOrientation(Gfx& gfx, bool toLandscape, bool persist);
  // The direction pair (soft-key slots 2 and 3). See Scene.h for the rule and
  // ReaderScene.cpp for why the text-size strip is the one exception.
  bool dirSwap() const;
  bool backKey(Input& in) const;
  bool fwdKey(Input& in) const;
  void toggleBookmarkHere();
  bool bookmarkHereIndex(int* idxOut);
  void loadBookmarks();
  void jumpToBookmark(int idx);
  void renderBody(Gfx& gfx);
  void renderReading(Gfx& gfx);
  void renderReadingChrome(Gfx& gfx);
  void renderBookList(Gfx& gfx);
  void renderTile(Gfx& gfx, int visibleIndex);
  void renderMessage(Gfx& gfx, const char* line1, const char* line2);
  void renderCoverageNotice(Gfx& gfx);

  State _state = State::BookList;
  Work _work = Work::None;
  bool _workArmed = false;
  bool _quietReopen = false;  // render() composed the frame announcing _work

  // Engine objects — created on open, freed in onExit (CrossPoint activity
  // model: reopening from the book.bin cache is fast).
  std::shared_ptr<reader::Epub> _epub;
  std::unique_ptr<reader::Section> _section;
  std::unique_ptr<reader::TextMeasure> _measure;
  // FBP mode: a compiled package is open instead of an Epub. The phone did
  // all layout; this path is page reads + blits only.
  std::unique_ptr<reader::FbpBook> _fbp;
  uint16_t _fbpPage = 0;
  uint16_t _lastPageCount = 0;  // last known pagination, for the place we hand the phone at close
  bool _fbpPosPending = false;  // .pos not read yet: needs the profile's page count
  bool _oomRestartPending = false;  // "Freeing memory" is on glass; restart on the next input pump
  void workOpenFbp();
  void fbpTurn(bool forward);
  void renderFbp(Gfx& gfx);
  reader::BookTextRenderer _renderer;
  reader::ReaderSettings _settings;
  std::string _bookPath;

  // True once BLE is suspended and the 32 KB dict + cover scratch are claimed
  // for book work (open / tile builds). The cached grid runs radio-up; this
  // flips at the first real work and onExit resumes the radio.
  bool _radioSuspended = false;
  // Framebuffer pointer captured in render() for the runWork() dict loan
  // (the static BSS framebuffer doubles as the 32 KB inflate window while
  // blocking work runs — flush is idle and render() repaints after).
  uint8_t* _fbForLoan = nullptr;
  void suspendRadioForBookWork();
  void streamPos();

  int _spine = 0;
  uint16_t _nextPage = 0;  // page to apply when the section (re)loads
  // Font-size cycle position restore: ratio -> page needs the NEW pageCount,
  // which only exists after the section (re)loads (applyPendingPage).
  float _pendingRatio = 0.0f;
  bool _hasPendingRatio = false;
  bool _hadProgress = false;
  int _prefetchAttemptedSpine = -1;  // never retry a failed/checked prefetch
  int _pageLoadRetries = 0;
  const char* _errorMsg = "";
  // Glyph-coverage notice (flowe-os#3 UX): when the first indexed chapter of
  // a raw epub is largely outside the built-in fonts (Hebrew, Arabic, CJK),
  // one full-screen notice points the user at the app's compile path instead
  // of pages of replacement boxes. Any button reads anyway.
  bool _coverageChecked = false;
  bool _coverageNotice = false;
  // A3 finished-book offer: NEXT pressed on the last page of a package means
  // the book is done, so offer the book that has waited longest on the card
  // (earliest-modified unread .fbp — no .pos sidecar and readable metadata).
  // Device-side only; nothing crosses the radio. No candidate = no screen.
  bool _endOffer = false;
  char _endOfferPath[160] = {0};
  char _endOfferTitle[64] = {0};
  char _endOfferAuthor[48] = {0};
  uint32_t _offerGotoCid = 0;   // X1: land here instead of the saved place
  bool _offerFromPhone = false; // X1: the offer screen is a phone-jump confirm
  // C4 end of book (docs/plans/2026-09-04-reader-quality-plan.md). The
  // screen now opens with or without a next book. Row 0 is "Mark
  // finished": it writes "<book>.done" beside the package (the phones read
  // it from the shelf listing and fold the book as 100%), ends the
  // session and returns to the shelf. Row 1 opens the next unread book.
  // Turning a page in a finished book removes the sidecar: reading again
  // un-finishes it.
  int _endSel = 0;
  bool _bookDone = false;
  void markFinished();
  // Word cursor (2026-09-05, docs/plans/2026-09-04-reader-quality-plan.md
  // "Cursor"). Menu > Look up. The cursor lands on the v8 word boxes of the
  // page: Left/Right step words (hold to repeat), Up/Down step lines to the
  // nearest word, past the page edge the page turns. GO opens the word
  // sheet: the word, "Open note" when the word is a footnote mark, and the
  // dictionary line (no dictionary on the card yet). BACK leaves.
  static constexpr uint16_t kMaxPageWords = 400;
  bool _wcMode = false;
  bool _wcSheet = false;
#ifdef FLOWE_PREPARED_GUIDE
  bool _wcGuide = false;
  bool _guideBound = false;
  uint8_t _guideHash[32] = {};
  uint32_t _guideCid = 0;
  void openWordGuide();
  void renderWordGuide(Gfx& gfx);
#endif
  bool _wcNeedLoad = false;   // words load in render(), after the page draws
  int8_t _wcPendingEdge = 0;  // +1: land on the first word, -1: on the last
  bool _wcNote = false;       // pre-v8 book: the row's value column explains
  int _wcIdx = 0;
  uint16_t _wcCount = 0;
  uint32_t _wcRepeatNextMs = 0;
  reader::FbpBook::WordBox _wcWords[kMaxPageWords];
  char _wcWord[48] = {0};
  // The dictionary's answer for the sheet (reader/Dictionary.h): the
  // headword it matched, the entry text, and how far the text is scrolled.
  bool _wcDictPresent = false;
  bool _wcDefFound = false;
  char _wcHead[48] = {0};
  char _wcDef[700] = {0};
  int _wcScroll = 0;
  void enterWordMode();
  void handleWordInput(Input& in);
  void wordLoad();
  void wordMove(int dir);       // -1 / +1 along the page, page turn at the edges
  void wordMoveLine(int dir);   // -1 up / +1 down, nearest word by x
  int wordNoteIndex();          // note on this page for the cursor word, -1 = none
  void renderWordCursor(Gfx& gfx);
  void renderWordSheet(Gfx& gfx);
  // R1 highlight pick mode.
  bool _hlMode = false;
  bool _hlNote = false;         // pre-v7 row pressed: value explains
  uint32_t _hlCid = 0;
  int8_t _hlPendingEdge = 0;    // +-1: select first/last ¶ after an edge turn
  uint32_t _hlFlashUntil = 0;
  char _hlFlashText[24] = {0};
  void enterHlMode();
  void handleHlInput(Input& in);
  void offerNextUnread();
  bool findOldestUnread(char* pathOut, size_t pathCap);
  void openEndOfferBook();
  void renderEndOffer(Gfx& gfx);

  // 0.7 reader chrome v2: the MENU is a full page (CONFIRM opens while
  // reading); Text size and Go-to-page are strips OVER the book page so
  // the result previews live; Chapters is its own full page.
  enum class MenuView : uint8_t { None, Page, SizeStrip, Chapters, GoTo, Bookmarks,
                                 StatsBook, StatsLife, Notes };
  MenuView _menu = MenuView::None;
  int _menuSel = 0;    // cursor on the menu page (persists while book open)
  // Orientation row, pressed on a book with no landscape pages: the value
  // column swaps to name the fix. Cleared whenever the menu closes or the
  // cursor moves, so it never becomes a permanent label.
  bool _orientNote = false;
  int _tocSel = 0;     // cursor in the Chapters list
  int _tocScroll = 0;  // first visible Chapters row
  // Chapters auto-repeat (see handleInput): which direction is held, when the
  // next repeat is due, and whether a held scroll moved without painting yet.
  Btn _tocRepeatFrom = Btn::COUNT;
  uint32_t _tocRepeatNextMs = 0;
  int _gotoPage = 0;   // 0-based target while the Go-to strip is open
  // Bookmarks for the open book (loaded on open, kept in sync on edit).
  reader::Bookmarks::Mark _marks[reader::Bookmarks::kMax];
  int _markCount = 0;
  int _markSel = 0;
  int _markScroll = 0;
  // C3 footnotes (docs/plans/2026-09-04-reader-quality-plan.md). The menu
  // offers "Footnotes" only on a page whose paragraphs carry a mark; GO
  // jumps to the note and the BOOKS key becomes RETURN until it is
  // pressed. Page turns keep the return alive (read the whole notes
  // section, then come back); chapter, bookmark and go-to jumps drop it.
  int notesHere(uint32_t* first);  // count on the current page (cached)
  void noteJump(int idx);
  void noteReturn();
  void renderNotes(Gfx& gfx);
  int _noteSel = 0;
  uint16_t _noteCachePage = 0xFFFF;
  uint16_t _noteCachePages = 0;  // pageCount the cache was built for
  bool _noteCacheExact = false;  // built from the page's own marks (word boxes)
  uint32_t _noteCacheFirst = 0;
  int _noteCacheCount = 0;
  int32_t _noteReturn = -1;      // page to return to, -1 = no jump active
  // Shelf: the stats band is selectable (_sel == -1, the Notifications
  // header-pill idiom) and opens READING LIFE — your reading belongs to
  // the shelf, not to whichever book happens to be open.
  bool _lifeOpen = false;
  // flowe-os#44: RESET on the READING LIFE page arms a full-page confirm;
  // ERASE there wipes the store. Disarmed on every path that leaves the page.
  bool _statsResetArm = false;
  // flowe-os#41: draw the soft-key bar while reading a page. Mirrored from
  // NVS "rdKeys"; the shelf and every menu view always keep their labels.
  bool _readKeyBar = true;
  // Landscape reading (FBP only — the package must carry the profiles).
  bool _landscape = false;
  // The reader's GLOBAL orientation preference, mirrored from NVS "rdLand".
  // Distinct from _landscape, which is what the panel is doing right now:
  // a portrait-only book reads portrait without disturbing the preference.
  bool _wantLandscape = false;
  // Set in render() (where the panel size is known) and read by the menu
  // input handlers, which have no Gfx of their own.
  bool _landscapeReady = false;
  // -1 = nothing pending, 0 = go portrait, 1 = go landscape. render() applies
  // it because that is where the Gfx is. Persist only when the READER asked
  // (the menu row) — never when we are merely restoring what was saved.
  int8_t _pendingOrient = -1;
  bool _pendingOrientPersist = false;
  bool _pendingRestorePortrait = false;
  // Session reading pace for "time left": EMA of ms between FORWARD page
  // turns (3 s – 3 min window so pauses and skimming don't poison it).
  // 0 = no data yet this session.
  uint32_t _lastTurnMs = 0;
  uint32_t _avgTurnMs = 0;

  BookEntry _books[kMaxBooks];  // ~9KB BSS (scene is a static instance)
  int _bookCount = 0;       // entries STORED in the window (<= kMaxBooks)
  int _sel = 0;             // absolute index into the whole library
  int _scroll = 0;          // absolute, row-aligned
  int _totalBooks = 0;      // every match on the card, stored or not (or the view size, see shelfSplit)
  int _windowOffset = 0;    // absolute index of _books[0]
  int _liveCount = 0;       // #26: unfinished entries at the front of _books
  int _doneCount = 0;       // #26: finished entries at the tail of _books
  bool _doneView = false;   // #26: the Done shelf is open
  uint16_t _skippedNames = 0;  // files skipped: name/path too long (logged on serial)
  bool _sdOk = true;        // card mounted when the scan ran
  int16_t _wCache = 0;
  int16_t _hCache = 0;
};
