import { motion } from 'motion/react'
import { Circle, Cpu, Download, Radio, Square, Usb } from 'lucide-react'
import { useEffect, useState } from 'react'
import { toast } from 'sonner'
import { Button } from '@/components/ui/button'
import { Kbd } from '@/components/ui/kbd'
import {
  Select, SelectContent, SelectGroup, SelectItem, SelectLabel, SelectSeparator, SelectTrigger,
} from '@/components/ui/select'
import {
  Sheet, SheetContent, SheetDescription, SheetHeader, SheetTitle, SheetTrigger,
} from '@/components/ui/sheet'
import { Tooltip, TooltipContent, TooltipTrigger } from '@/components/ui/tooltip'
import { api, estop } from '@/lib/api'
import { useStore } from '@/lib/store'
import { cn } from '@/lib/utils'

export const PAGES = [
  { id: 'drive', label: 'Drive' },
  { id: 'scope', label: 'Scope' },
  { id: 'tests', label: 'Tests' },
  { id: 'params', label: 'Parameters' },
  { id: 'console', label: 'Console' },
] as const
export type PageId = (typeof PAGES)[number]['id']

export function TopBar({ page, onPage }: { page: PageId; onPage: (p: PageId) => void }) {
  const wsUp = useStore((s) => s.wsUp)
  const running = useStore((s) => !!s.tel && s.tel.st !== 'idle')

  return (
    <header className="sticky top-0 z-40 border-b border-stone/70 bg-canvas/95 backdrop-blur-[2px]">
      <div className="flex h-14 items-center gap-8 px-4 sm:px-6 2xl:px-10">
        <div className="flex items-center gap-2.5">
          <Cpu className="size-4" strokeWidth={1.75} />
          <span className="font-sans text-[12px] font-bold tracking-[0.18em] uppercase">BLDC · FOC</span>
        </div>

        <nav className="hidden items-center gap-1 md:flex">
          {PAGES.map((p) => (
            <button
              key={p.id}
              onClick={() => onPage(p.id)}
              className={cn(
                'relative h-14 px-3 font-sans text-[13px] font-medium transition-colors',
                page === p.id ? 'text-slate' : 'text-cloud-dark hover:text-slate',
              )}
            >
              {p.label}
              {page === p.id && (
                <motion.span
                  layoutId="nav-underline"
                  className="absolute inset-x-3 -bottom-px h-[2px] bg-slate"
                  transition={{ type: 'spring', stiffness: 500, damping: 40 }}
                />
              )}
            </button>
          ))}
        </nav>

        <div className="ml-auto flex items-center gap-2">
          <Connection />
          <Recorder />
          <Tooltip>
            <TooltipTrigger asChild>
              <motion.div whileTap={{ scale: 0.96 }}>
                <Button
                  onClick={estop}
                  disabled={!wsUp}
                  className={cn(
                    'h-9 gap-2 rounded-md px-5 font-sans text-[13px] font-bold tracking-[0.08em]',
                    running ? 'bg-clay text-white hover:bg-clay-deep' : 'bg-slate text-ivory hover:bg-slate-medium',
                  )}
                >
                  <Square className="size-3.5 fill-current" />
                  STOP
                </Button>
              </motion.div>
            </TooltipTrigger>
            <TooltipContent>
              Bridge off now (ESC to the board). Keyboard: <Kbd>Esc</Kbd> or <Kbd>Space</Kbd>
            </TooltipContent>
          </Tooltip>
        </div>
      </div>
      {/* mobile nav */}
      <div className="flex gap-4 overflow-x-auto px-4 pb-2 md:hidden">
        {PAGES.map((p) => (
          <button
            key={p.id}
            onClick={() => onPage(p.id)}
            className={cn('border-b py-1 text-xs font-medium', page === p.id ? 'border-slate text-slate' : 'border-transparent text-cloud-dark')}
          >
            {p.label}
          </button>
        ))}
      </div>
    </header>
  )
}

function Connection() {
  const status = useStore((s) => s.status)
  const wsUp = useStore((s) => s.wsUp)
  const connected = !!status?.connected
  const value = status?.mode === 'serial' && status.port ? `serial:${status.port}` : status?.mode ?? 'auto'

  const choose = async (v: string) => {
    const [mode, ...rest] = v.split(':')
    try {
      await api.post('/api/connect', { mode, port: rest.join(':') || null })
    } catch (e) {
      toast.error(String((e as Error).message))
    }
  }

  const label = !wsUp
    ? 'Backend offline'
    : connected
      ? status?.link === 'sim' ? 'Simulator' : (status?.app ?? 'Board')
      : status?.paused ? 'Flashing…' : 'Searching…'

  return (
    <Select value={value} onValueChange={choose}>
      <SelectTrigger className="h-9 w-auto min-w-44 gap-2 rounded-md border-transparent bg-transparent font-sans text-xs shadow-none hover:bg-oat/60">
        <span className="relative flex size-2">
          {connected && <span className="absolute inline-flex size-full animate-ping rounded-full bg-good opacity-60" />}
          <span className={cn('relative size-2 rounded-full', connected ? 'bg-good' : wsUp ? 'bg-warn' : 'bg-clay-deep')} />
        </span>
        {status?.link === 'sim' ? <Radio className="size-3.5" /> : <Usb className="size-3.5" />}
        <span className="max-w-40 truncate">{label}</span>
      </SelectTrigger>
      <SelectContent align="end" className="min-w-72">
        <SelectGroup>
          <SelectLabel>Source</SelectLabel>
          <SelectItem value="auto">USB board (auto-connect)</SelectItem>
          <SelectItem value="sim">Simulator (no hardware)</SelectItem>
          <SelectItem value="off">Disconnected</SelectItem>
        </SelectGroup>
        {!!status?.ports?.length && (
          <>
            <SelectSeparator />
            <SelectGroup>
              <SelectLabel>Ports</SelectLabel>
              {status.ports.map((p) => (
                <SelectItem key={p.path} value={`serial:${p.path}`}>
                  {p.app ?? 'unknown app'} <span className="text-muted-foreground">({p.dev})</span>
                </SelectItem>
              ))}
            </SelectGroup>
          </>
        )}
      </SelectContent>
    </Select>
  )
}

interface Rec { name: string; bytes: number; mtime: number }

function Recorder() {
  const status = useStore((s) => s.status)
  const recording = !!status?.recording
  const [list, setList] = useState<Rec[]>([])
  const [open, setOpen] = useState(false)

  useEffect(() => {
    if (open) api.get<Rec[]>('/api/recordings').then(setList).catch(() => setList([]))
  }, [open, recording])

  const toggle = async () => {
    try {
      if (recording) {
        const r = await api.post<{ name: string }>('/api/recordings/stop')
        toast.success(`Saved ${r.name}`)
      } else {
        await api.post('/api/recordings/start')
      }
    } catch (e) {
      toast.error(String((e as Error).message))
    }
  }

  return (
    <div className="hidden items-center gap-1 border-x border-stone/70 px-2 sm:flex">
      <Tooltip>
        <TooltipTrigger asChild>
          <Button variant="ghost" onClick={toggle} className="h-9 gap-2 rounded-md px-3 text-xs hover:bg-oat/60">
            <Circle className={cn('size-3', recording ? 'animate-pulse fill-clay text-clay' : 'fill-cloud text-cloud')} />
            {recording ? <span className="num">{status?.rec_rows ?? 0} rows</span> : 'Record'}
          </Button>
        </TooltipTrigger>
        <TooltipContent>Record every telemetry sample to CSV</TooltipContent>
      </Tooltip>
      <Sheet open={open} onOpenChange={setOpen}>
        <SheetTrigger asChild>
          <Button variant="ghost" className="h-9 rounded-md px-2.5 hover:bg-oat/60">
            <Download className="size-3.5" />
          </Button>
        </SheetTrigger>
        <SheetContent className="bg-canvas">
          <SheetHeader>
            <SheetTitle className="font-voice text-xl">Recordings</SheetTitle>
            <SheetDescription>Telemetry CSV files in backend/recordings/.</SheetDescription>
          </SheetHeader>
          <div className="flex flex-col gap-2 px-4">
            {list.length === 0 && <p className="font-voice text-muted-foreground">Nothing recorded yet.</p>}
            {list.map((r) => (
              <a
                key={r.name}
                href={`/api/recordings/${r.name}`}
                className="hair-top flex items-center justify-between px-1 py-3 transition-colors hover:bg-oat/50"
              >
                <span className="num text-xs">{r.name}</span>
                <span className="text-xs text-muted-foreground">{(r.bytes / 1024).toFixed(1)} KB</span>
              </a>
            ))}
          </div>
        </SheetContent>
      </Sheet>
    </div>
  )
}
