import { CornerDownLeft, Trash2 } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { Button } from '@/components/ui/button'
import { Card, CardContent } from '@/components/ui/card'
import { Input } from '@/components/ui/input'
import { Switch } from '@/components/ui/switch'
import { cmd } from '@/lib/api'
import { useStore } from '@/lib/store'
import { cn } from '@/lib/utils'
import { SectionTitle } from './common'

const QUICK = ['help', 'params', 'csa', 'hello', 'trace 50', 'stop']

export function ConsolePage() {
  const lines = useStore((s) => s.console)
  const clear = useStore((s) => s.clearConsole)
  const [text, setText] = useState('')
  const [hist, setHist] = useState<string[]>([])
  const [hi, setHi] = useState(-1)
  const [follow, setFollow] = useState(true)
  const box = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (follow && box.current) box.current.scrollTop = box.current.scrollHeight
  }, [lines, follow])

  const send = (l: string) => {
    const line = l.trim()
    if (!line) return
    cmd(line)
    setHist((h) => [line, ...h.filter((x) => x !== line)].slice(0, 50))
    setHi(-1)
    setText('')
  }

  return (
    <div className="flex flex-col gap-5">
      <SectionTitle eyebrow="Raw" title="Console">
        <div className="flex items-center gap-3">
          <label className="flex items-center gap-2 font-sans text-xs font-medium">
            Follow <Switch checked={follow} onCheckedChange={setFollow} />
          </label>
          <Button variant="outline" size="sm" className="gap-1.5 rounded-xl" onClick={clear}>
            <Trash2 className="size-3.5" />Clear
          </Button>
        </div>
      </SectionTitle>
      <Card className="gap-0 overflow-hidden bg-slate py-0 ring-0">
        <CardContent className="p-0">
          <div ref={box} className="h-[60vh] overflow-y-auto px-5 py-4">
            <pre className="num text-[12px] leading-relaxed whitespace-pre-wrap">
              {lines.map((l, k) => (
                <div key={k} className={cn(
                  l.startsWith('›') ? 'text-manilla' : /RESULT|stopped|FAIL|fault|LOW|refuse|\?/.test(l) ? 'text-clay' : 'text-ivory/85',
                )}>{l}</div>
              ))}
            </pre>
          </div>
          <form className="flex items-center gap-2 border-t border-slate-medium bg-slate px-4 py-3"
            onSubmit={(e) => { e.preventDefault(); send(text) }}>
            <span className="num text-manilla">›</span>
            <Input
              value={text}
              onChange={(e) => setText(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'ArrowUp' && hist.length) {
                  e.preventDefault()
                  const n = Math.min(hi + 1, hist.length - 1); setHi(n); setText(hist[n])
                } else if (e.key === 'ArrowDown') {
                  e.preventDefault()
                  const n = hi - 1; setHi(n); setText(n >= 0 ? hist[n] : '')
                }
              }}
              placeholder="command, e.g. amp 30   (help lists them)"
              className="num h-9 border-none bg-transparent text-ivory shadow-none placeholder:text-cloud-dark focus-visible:ring-0"
            />
            <Button type="submit" size="sm" variant="ivory" className="gap-1.5"><CornerDownLeft className="size-3.5" />Send</Button>
          </form>
        </CardContent>
      </Card>
      <div className="flex flex-wrap gap-2">
        {QUICK.map((q) => (
          <Button key={q} variant="outline" size="sm" className="num rounded-xl border-stone bg-ivory" onClick={() => send(q)}>{q}</Button>
        ))}
      </div>
      <p className="font-voice text-sm text-muted-foreground">
        Commands go through the backend's allow-list. Machine lines (@T, @P …) are parsed and hidden here.
      </p>
    </div>
  )
}
