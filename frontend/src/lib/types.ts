// Mirrors the firmware '@' protocol (firmware/tests/test_08_drive_lab/main.c)
// and the backend events (backend/app/hub.py).

export type RunState = 'idle' | 'align' | 'run' | 'raw' | 'rl' | 'foc'
export type Mod = '6step' | 'sine' | 'svpwm' | 'dpwm'
export type Triple = [number, number, number]

/** @T telemetry */
export interface Tel {
  t: number
  st: RunState
  mod: Mod
  kind: 'if' | 'sl'
  obs: number
  ms: number
  f: number
  fol: number
  fpll: number
  amp: number
  cap: number
  ifilt: number
  i: Triple
  iidle: Triple
  rms: Triple
  pk: Triple
  res: number
  flow: number
  drop: number
  id: number
  iq: number
  idr: number
  iqr: number
  vd: number
  vq: number
  lam: number
  lamu: number
  d: Triple
  step: number
  th: number
  tho: number
  nf: number
  fmax: number
  imax: number
  off: Triple
  bad: number
  stop: string
}

/** @P parameters (inputs) */
export interface Params {
  vbus: number
  ilim: number
  amp: number
  ampmax: number
  f: number
  fstart: number
  acc0: number
  acc1: number
  align: number
  mode: Mod
  R: number
  L: number
  lam: number
  bw: number
  ifa: number
  fsw: number
  kps: number
  kis: number
  pll: number
  idsl: number
  vrec: number
  dtc: number
  focmod: Mod
  trdec: number
  log: number
  tel: number
}

export interface Hello {
  app: string
  proto: number
  fs: number
  csa: number
  itrip: number
  trn: number
  built: string
  sim?: number
}

export interface Csa {
  off: Triple
  ok: Triple
  bad: number
  valid: number
}

export interface Wave {
  dt_us: number
  st: string
  frozen: number
  n: number
  ia: number[]
  ib: number[]
  ic: number[]
  da: number[]
  db: number[]
  dc: number[]
  th: number[]
  tho: number[]
  valid: number[]
  nf: number[]
}

export interface Port {
  path: string
  app: string | null
  dev: string
}

export interface JobBrief {
  id: string
  test: string
  title: string
  status: string
  phase: string
  step: number
  steps: number
}

export interface Status {
  connected: boolean
  mode: 'auto' | 'serial' | 'sim' | 'off'
  link: 'serial' | 'sim' | null
  port: string | null
  app: string | null
  paused: boolean
  error: string
  ports: Port[]
  recording: string | null
  rec_rows: number
  job: JobBrief | null
}

export interface RunResult {
  kind: string
  why: string
  fmax: number
  imax: number
}

export interface RawResult {
  d: Triple
  mean: Triple
  flow: Triple
  why: string
}

export interface RlResult {
  ok: number
  R?: number
  L1k?: number
  L2k?: number
  L?: number
  why?: string
}

export interface Job {
  id: string
  test: string
  title: string
  status: 'running' | 'passed' | 'failed' | 'done' | 'error' | 'cancelled'
  step: number
  steps: number
  phase: string
  log: string[]
  results: Array<Record<string, unknown> & { kind: string }>
  metrics: Array<{ label: string; value: string; unit: string }>
  values: Record<string, number>
  started: number
  ended: number | null
  verdict: string | null
}

export interface TestInput {
  key: string
  label: string
  default: number
  min: number
  max: number
  step: number
  unit: string
}

export interface TestDef {
  id: string
  kind: 'app' | 'lab'
  group: string
  title: string
  safety: 'safe' | 'twitch' | 'motor'
  covers: string
  app?: string
  secs?: number
  inputs?: TestInput[]
  metrics?: string[]
  steps?: number
}

export interface LogEvent {
  id: number
  at: number
  level: 'info' | 'warn' | 'error' | 'ok'
  text: string
}
