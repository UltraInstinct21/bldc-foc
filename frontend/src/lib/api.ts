import { toast } from 'sonner'
import { useStore, type BackendEvent } from './store'

async function req<T>(method: string, path: string, body?: unknown): Promise<T> {
  const r = await fetch(path, {
    method,
    headers: body ? { 'content-type': 'application/json' } : undefined,
    body: body ? JSON.stringify(body) : undefined,
  })
  if (!r.ok) {
    let msg = `${r.status} ${r.statusText}`
    try {
      const j = await r.json()
      if (j.detail) msg = typeof j.detail === 'string' ? j.detail : JSON.stringify(j.detail)
    } catch { /* not json */ }
    throw new Error(msg)
  }
  return r.json() as Promise<T>
}

export const api = {
  get: <T,>(p: string) => req<T>('GET', p),
  post: <T,>(p: string, b?: unknown) => req<T>('POST', p, b ?? {}),
}

// ---- WebSocket -----------------------------------------------------------------
let ws: WebSocket | null = null
let retry = 0

export function connectWs() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws'
  ws = new WebSocket(`${proto}://${location.host}/ws`)
  const { ingest } = useStore.getState()
  ws.onopen = () => {
    retry = 0
    useStore.setState({ wsUp: true })
  }
  ws.onmessage = (m) => {
    try {
      const ev = JSON.parse(m.data) as BackendEvent
      if (ev.type === 'event' && ev.level === 'error') toast.error(ev.text)
      ingest(ev)
    } catch { /* ignore malformed */ }
  }
  ws.onclose = () => {
    useStore.setState({ wsUp: false })
    setTimeout(connectWs, Math.min(4000, 300 * 2 ** retry++))
  }
}

/** Send a console command. Uses the socket when it is up (lowest latency). */
export function cmd(line: string) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ type: 'cmd', line }))
    return
  }
  api.post('/api/command', { line }).catch((e) => toast.error(String(e.message ?? e)))
}

/** Emergency stop: socket and REST both, whichever arrives first. */
export function estop() {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ type: 'stop' }))
  api.post('/api/stop').catch(() => toast.error('Stop could not reach the backend: unplug the supply'))
}

/** Rate-limit a live control (slider) to one command per `ms`, always sending the last value. */
export function throttled(ms: number) {
  let last = 0
  let timer: ReturnType<typeof setTimeout> | null = null
  let pending: string | null = null
  return (line: string) => {
    pending = line
    const now = Date.now()
    const fire = () => {
      if (pending) cmd(pending)
      pending = null
      last = Date.now()
      timer = null
    }
    if (now - last >= ms) fire()
    else if (!timer) timer = setTimeout(fire, ms - (now - last))
  }
}
