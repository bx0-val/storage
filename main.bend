import Base
import ./app.bend as App

def Native.init() -> IO(App.Decision):
  import "./native.c"

def Native.action(action: App.Action, state: App.State) -> IO(App.State):
  import "./native.c"

def Native.frame(state: App.State) -> IO(App.Frame):
  import "./native.c"

def Native.write(text: String) -> IO(Unit):
  import "./native.c"

def Native.key() -> IO(App.Key):
  import "./native.c"

def Native.close() -> IO(Unit):
  import "./native.c"

def present(+state: App.State, +frame: App.Frame) -> IO(App.State):
  +current = App.bound(state, App.count(frame))
  do IO<App.State>:
    Native.write(App.render(current, frame))
    return current

def run(fuel: Nat, decision: App.Decision) -> IO(Unit):
  match fuel decision:
    case _ App.Stop{}: Native.close()
    case 0n _: Native.close()
    case 1n+p App.Continue{state, action}:
      do IO<Unit>:
        +next : App.State <- Native.action(action, state)
        frame : App.Frame <- Native.frame(next)
        current : App.State <- present(next, frame)
        key : App.Key <- Native.key()
        run(p, App.step(key, current))

def main() -> IO(Unit):
  do IO<Unit>:
    decision : App.Decision <- Native.init()
    run(4294967295n, decision)
