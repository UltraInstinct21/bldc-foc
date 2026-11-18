import { create } from 'zustand'
import type {
  Csa, Hello, Job, LogEvent, Params, RawResult, RlResult, RunResult, Status, Tel, Wave,
} from './types'

const HIST_MAX = 600            // 30 s at the 50 ms telemetry period
const CONSOLE_MAX = 1500
const EVENTS_MAX = 40

export interface HistPoint extends Tel {
  ts: number                    // seconds, relative to the first sample
}

interface State {
  wsUp: boolean
  status: Status | null
  params: Params | null
  hello: Hello | null
  csa: Csa | null
  tel: Tel | null
  hist: HistPoint[]
  console: string[]
  wave: Wave | null
  job: Job | null
  lastResult: RunResult | null
  raws: RawResult[]
  rl: RlResult | null
  events: LogEvent[]
  polePairs: number
  t0: number | null
  setPolePairs: (n: number) => void
  clearHist: () => void
  clearConsole: () => void
  pushEvent: (level: LogEvent['level'], text: string) => void
  ingest: (ev: BackendEvent) => void
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export type BackendEvent = { type: string; [k: string]: any }

let evId = 0

function readPolePairs(): number {
  try {
    const v = Number(localStorage.getItem('polePairs'))
    return v > 0 ? v : 7
  } catch {
    return 7
  }
}

export const useStore = create<State>((set, get) => ({
  wsUp: false,
  status: null,
  params: null,
  hello: null,
  csa: null,
  tel: null,
  hist: [],
  console: [],
  wave: null,
  job: null,
  lastResult: null,
  raws: [],
  rl: null,
  events: [],
  polePairs: readPolePairs(),
  t0: null,

  setPolePairs: (n) => {
    try { localStorage.setItem('polePairs', String(n)) } catch { /* private window */ }
    set({ polePairs: n })
  },
  clearHist: () => set({ hist: [], t0: null }),
  clearConsole: () => set({ console: [] }),
  pushEvent: (level, text) =>
    set((s) => ({ events: [{ id: ++evId, at: Date.now(), level, text }, ...s.events].slice(0, EVENTS_MAX) })),

  ingest: (ev) => {
    const s = get()
    switch (ev.type) {
      case 'snapshot':
        set({
          status: ev.status, params: ev.params, hello: ev.hello, csa: ev.csa, tel: ev.tel, wave: ev.wave ?? null,
          console: ev.console ?? [],
        })
        break
      case 'status':
        set({ status: { ...(s.status ?? {}), ...ev } as Status })
        break
      case 'tel': {
        const d = ev.data as Tel
        let t0 = s.t0 ?? d.t
        const last = s.hist[s.hist.length - 1]
        // a board reset restarts the clock: start a fresh history
        if (last && (d.t - t0) / 1000 < last.ts) t0 = d.t
        const p: HistPoint = { ...d, ts: (d.t - t0) / 1000 }
        const hist = t0 === d.t ? [p] : [...s.hist, p].slice(-HIST_MAX)
        set({ tel: d, hist, t0 })
        break
      }
      case 'params':
        set({ params: ev.data })
        break
      case 'hello':
        set({ hello: ev.data })
        break
      case 'csa':
        set({ csa: ev.data })
        break
      case 'wave':
        set({ wave: ev.data })
        break
      case 'result': {
        const r = ev.data as RunResult
        set({ lastResult: r })
        const benign = /ESC|stop command|UI stop|deadman/.test(r.why)
        s.pushEvent(benign ? 'info' : 'warn',
          `Run ended (${r.kind}): ${r.why}. Max ${r.fmax.toFixed(1)} Hz, ${r.imax.toFixed(2)} A`)
        break
      }
      case 'raw':
        set({ raws: [...s.raws, ev.data].slice(-20) })
        break
      case 'rl': {
        const r = ev.data as RlResult
        set({ rl: r })
        s.pushEvent(r.ok ? 'ok' : 'error', r.ok
          ? `Measured R = ${(r.R ?? 0).toFixed(4)} Ω, L = ${(r.L ?? 0).toFixed(1)} µH`
          : `R/L measurement failed: ${r.why}`)
        break
      }
      case 'console':
      case 'sent': {
        const line = ev.type === 'sent' ? `› ${ev.line}` : ev.line
        set({ console: [...s.console, line].slice(-CONSOLE_MAX) })
        break
      }
      case 'job':
        set({ job: ev.data })
        break
      case 'job_log': {
        const j = s.job
        if (j && j.id === ev.id) set({ job: { ...j, log: [...j.log, ev.line].slice(-500) } })
        break
      }
      case 'event':
        s.pushEvent(ev.level ?? 'info', ev.text)
        break
    }
  },
}))

export const rpmOf = (fHz: number, pp: number) => (fHz * 60) / pp
