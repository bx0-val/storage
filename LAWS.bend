# Behavioral requirements for the actual application state machine.
# Changes to these requirements must be deliberate; do not weaken a law to pass.
import Base
import ./app.bend as App

law ticks_never_scan:
  for +s: App.State
  {App.step(App.Tick{}, s) == App.Continue{s, App.Stay{}} : App.Decision}

law quit_always_stops:
  for s: App.State
  {App.step(App.Quit{}, s) == App.Stop{} : App.Decision}

law folder_refresh_is_explicit:
  for +n: U32
  for +a: Bool
  for +h: Bool
  {App.step(App.Refresh{}, App.State{App.Folders{}, n, a, h}) ==
   App.Continue{App.State{App.Folders{}, n, a, h}, App.Rescan{}} : App.Decision}

law overview_refresh_never_scans:
  {App.refresh_action(App.Overview{}) == App.Restat{} : App.Action}

law disk_refresh_never_scans:
  {App.refresh_action(App.Disks{}) == App.Restat{} : App.Action}

law tabs_cycle_without_loss:
  for +v: App.Tab
  {App.next_tab(App.next_tab(App.next_tab(v))) == v : App.Tab}

law opening_folders_reuses_scan:
  for v: App.Tab
  for n: U32
  for +a: Bool
  for h: Bool
  {App.step(App.ShowFolders{}, App.State{v, n, a, h}) ==
   App.Continue{App.State{App.Folders{}, 0, a, False{}}, App.Stay{}} : App.Decision}

law home_resets_selection:
  for v: App.Tab
  for n: U32
  for +a: Bool
  for h: Bool
  {App.step(App.Home{}, App.State{v, n, a, h}) ==
   App.Continue{App.State{App.Folders{}, 0, a, False{}}, App.HomeDir{}} : App.Decision}

law first_resets_selection:
  for +v: App.Tab
  for n: U32
  for +a: Bool
  for +h: Bool
  {App.step(App.First{}, App.State{v, n, a, h}) ==
   App.Continue{App.State{v, 0, a, h}, App.Stay{}} : App.Decision}
