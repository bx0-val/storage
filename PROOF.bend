import Base
import ./LAWS.bend as Laws
import ./app.bend as App

def Laws.ticks_never_scan(s):
  match s:
    case App.State{v, n, a, h}: {==}

def Laws.copying_preserves_selection_and_view(s):
  match s:
    case App.State{v, n, a, h}: {==}

def Laws.quit_always_stops(s):
  match s:
    case App.State{v, n, a, h}: {==}

def Laws.folder_refresh_is_explicit(n, a, h): {==}
def Laws.overview_refresh_never_scans(): {==}
def Laws.disk_refresh_never_scans(): {==}

def Laws.tabs_cycle_without_loss(v):
  match v:
    case App.Overview{}: {==}
    case App.Disks{}: {==}
    case App.Folders{}: {==}

def Laws.opening_folders_reuses_scan(v, n, a, h): {==}
def Laws.home_resets_selection(v, n, a, h): {==}
def Laws.first_resets_selection(v, n, a, h): {==}
