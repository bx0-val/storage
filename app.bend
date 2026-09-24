import Base

type Tab is Data:
  Overview{}
  Disks{}
  Folders{}

type Key is Data:
  Tick{}
  Quit{}
  Cycle{}
  ShowOverview{}
  ShowDisks{}
  ShowFolders{}
  Up{}
  Down{}
  PageUp{}
  PageDown{}
  First{}
  Last{}
  Enter{}
  Back{}
  Home{}
  Refresh{}
  ToggleAll{}
  ToggleHelp{}

type Action is Data:
  Stay{}
  Open{}
  Parent{}
  HomeDir{}
  Rescan{}
  Restat{}

type State is Data:
  State{tab: Tab, selected: U32, all: Bool, help: Bool}

type Decision is Data:
  Stop{}
  Continue{state: State, action: Action}

type Row is Data:
  Row{index: U32, name: String, right: String, detail: String, percent: U32}

type Frame is Data:
  Frame{width: U32, height: U32, count: U32, title: String,
        summary: String, status: String, rows: List<&2, Row>}

def next_tab(tab: Tab) -> Tab:
  match tab:
    case Overview{}: Disks{}
    case Disks{}: Folders{}
    case Folders{}: Overview{}

def refresh_action(tab: Tab) -> Action:
  match tab:
    case Folders{}: Rescan{}
    case _: Restat{}

def earlier(n: U32, +step: U32) -> U32:
  (U32.max(n, step) - step : U32)

def later(n: U32, +step: U32) -> U32:
  (U32.min(n, (4294967295 - step : U32)) + step : U32)

def step(key: Key, state: State) -> Decision:
  match key state:
    case Quit{} _: Stop{}
    case Tick{} s: Continue{s, Stay{}}
    case Cycle{} State{tab, _, a, _}: Continue{State{next_tab(tab), 0, a, False{}}, Stay{}}
    case ShowOverview{} State{_, _, a, _}: Continue{State{Overview{}, 0, a, False{}}, Stay{}}
    case ShowDisks{} State{_, _, a, _}: Continue{State{Disks{}, 0, a, False{}}, Stay{}}
    case ShowFolders{} State{_, _, a, _}: Continue{State{Folders{}, 0, a, False{}}, Stay{}}
    case Up{} State{v, n, a, h}: Continue{State{v, earlier(n, 1), a, h}, Stay{}}
    case Down{} State{v, n, a, h}: Continue{State{v, later(n, 1), a, h}, Stay{}}
    case PageUp{} State{v, n, a, h}: Continue{State{v, earlier(n, 10), a, h}, Stay{}}
    case PageDown{} State{v, n, a, h}: Continue{State{v, later(n, 10), a, h}, Stay{}}
    case First{} State{v, _, a, h}: Continue{State{v, 0, a, h}, Stay{}}
    case Last{} State{v, _, a, h}: Continue{State{v, 4294967295, a, h}, Stay{}}
    case Enter{} s: Continue{s, Open{}}
    case Back{} s: Continue{s, Parent{}}
    case Home{} State{_, _, a, _}: Continue{State{Folders{}, 0, a, False{}}, HomeDir{}}
    case Refresh{} State{+v, n, a, h}: Continue{State{v, n, a, h}, refresh_action(v)}
    case ToggleAll{} State{v, _, a, h}: Continue{State{v, 0, Bool.not(a), h}, Stay{}}
    case ToggleHelp{} State{v, n, a, h}: Continue{State{v, n, a, Bool.not(h)}, Stay{}}

def bound(state: State, count: U32) -> State:
  State{v, n, a, h} = state
  State{v, U32.min(n, earlier(count, 1)), a, h}

def position(state: State) -> U32:
  State{_, n, _, _} = state
  n

def count(frame: Frame) -> U32:
  Frame{_, _, n, _, _, _, _} = frame
  n

def at(y: U32, x: U32, text: String) -> String:
  "\u{1b}[" ++ U32.show(y) ++ ";" ++ U32.show(x) ++ "H" ++ text

def color_mid(mid: Bool) -> String:
  match mid:
    case True{}: "\u{1b}[33m"
    case False{}: "\u{1b}[32m"

def color_high(high: Bool, value: U32) -> String:
  match high:
    case True{}: "\u{1b}[31m"
    case False{}: color_mid(U32.is_ge(value, 75))

def color(+value: U32) -> String:
  color_high(U32.is_ge(value, 90), value)

def highlight(selected: Bool) -> String:
  match selected:
    case True{}: "\u{1b}[7m > "
    case False{}: "   "

def bar(+value: U32) -> String:
  +filled = (U32.min(value, 100) / 5 : U32)
  color(value) ++ String.repeat("━", U32.to_nat(filled)) ++ "\u{1b}[2m" ++
  String.repeat("─", U32.to_nat((20 - filled : U32))) ++ "\u{1b}[0m"

def render_rows(rows: List<&2, Row>, +selected: U32, +width: U32, +y: U32) -> String:
  match rows:
    case Nil{}: ""
    case Con{Row{index, name, right, detail, percent}, rest}:
      at(y, 2, highlight(U32.is_eq(index, selected)) ++ name ++ "\u{1b}[0m") ++
      at(y, earlier(width, 28), right) ++
      at((y + 1 : U32), 5, bar(percent) ++ "  \u{1b}[2m" ++ detail ++ "\u{1b}[0m") ++
      render_rows(rest, selected, width, (y + 3 : U32))

def nav(tab: Tab) -> String:
  match tab:
    case Overview{}: "\u{1b}[7m 1 Overview \u{1b}[0m    2 Disks        3 Folders"
    case Disks{}: " 1 Overview     \u{1b}[7m 2 Disks \u{1b}[0m       3 Folders"
    case Folders{}: " 1 Overview      2 Disks        \u{1b}[7m 3 Folders \u{1b}[0m"

def content(help: Bool, rows: List<&2, Row>, selected: U32, width: U32) -> String:
  match help:
    case False{}: render_rows(rows, selected, width, 10)
    case True{}:
      at(10, 3, "1 / 2 / 3 or Tab    Switch views") ++
      at(11, 3, "Arrows or j / k     Select; Enter opens a folder") ++
      at(12, 3, "Backspace / h       Parent; H opens your home") ++
      at(13, 3, "r                   Refresh; a toggles virtual mounts") ++
      at(14, 3, "?                   Help; q / Esc quits") ++
      at(16, 3, "Scans are parallel. Cached folders open immediately.")

def render_large(small: Bool, state: State, frame: Frame) -> String:
  match small:
    case True{}:
      "\u{1b}[H\u{1b}[2J" ++ at(2, 2, "STORAGE") ++ at(4, 2, "Resize to at least 64 x 20, or use --plain.")
    case False{}:
      State{tab, +selected, _, help} = state
      Frame{+width, +height, total, title, summary, status, rows} = frame
      "\u{1b}[H\u{1b}[2J" ++ at(2, 3, "\u{1b}[1;36mSTORAGE\u{1b}[0m   device storage") ++
      at(4, 3, nav(tab)) ++ at(6, 3, "\u{1b}[1m" ++ title ++ "\u{1b}[0m") ++
      at(7, 3, summary) ++
      content(help, rows, selected, width) ++
      at(earlier(height, 3), 3, "\u{1b}[2m" ++ U32.show(total) ++ " entries  |  " ++ status ++ "\u{1b}[0m") ++
      at(earlier(height, 1), 3, "\u{1b}[36mq Quit   Tab View   Enter Open   r Refresh   ? Help\u{1b}[0m")

def render(+state: State, frame: Frame) -> String:
  Frame{+w, +h, n, title, summary, status, rows} = frame
  render_large(U32.is_lt(w, 64) || U32.is_lt(h, 20), state, Frame{w, h, n, title, summary, status, rows})
