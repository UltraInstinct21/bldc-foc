import { AnimatePresence, motion } from 'motion/react'
import { useEffect, useState } from 'react'
import { Toaster } from '@/components/ui/sonner'
import { TooltipProvider } from '@/components/ui/tooltip'
import { ConsolePage } from '@/components/ConsolePage'
import { DrivePage } from '@/components/drive/DrivePage'
import { ParamsPage } from '@/components/ParamsPage'
import { ScopePage } from '@/components/ScopePage'
import { TestsPage } from '@/components/TestsPage'
import { PAGES, TopBar, type PageId } from '@/components/TopBar'
import { connectWs, estop } from '@/lib/api'
import { useStore } from '@/lib/store'

function initialPage(): PageId {
  const h = location.hash.replace('#', '')
  return (PAGES.some((p) => p.id === h) ? h : 'drive') as PageId
}

export default function App() {
  const [page, setPage] = useState<PageId>(initialPage)
  const wsUp = useStore((s) => s.wsUp)

  useEffect(() => { connectWs() }, [])
  useEffect(() => { history.replaceState(null, '', `#${page}`) }, [page])

  // Esc always stops; Space stops unless the user is typing.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const typing = e.target instanceof HTMLElement &&
        (e.target.closest('input, textarea, [contenteditable="true"]') !== null)
      if (e.key === 'Escape' || (e.key === ' ' && !typing && !(e.target instanceof HTMLButtonElement))) {
        if (e.key === ' ') e.preventDefault()
        estop()
      }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [])

  return (
    <TooltipProvider delayDuration={250}>
      <div className="min-h-screen bg-canvas">
        <TopBar page={page} onPage={setPage} />
        <AnimatePresence>
          {!wsUp && (
            <motion.div initial={{ height: 0 }} animate={{ height: 'auto' }} exit={{ height: 0 }}
              className="overflow-hidden bg-slate text-center font-sans text-xs text-ivory">
              <div className="py-2">Backend not reachable. Start it with <code className="num">cd backend && uv run uvicorn app.main:app</code>. Retrying…</div>
            </motion.div>
          )}
        </AnimatePresence>
        <main className="px-4 py-8 sm:px-6 2xl:px-10">
          <AnimatePresence mode="wait">
            <motion.div
              key={page}
              initial={{ opacity: 0, y: 8 }}
              animate={{ opacity: 1, y: 0 }}
              exit={{ opacity: 0, y: -6 }}
              transition={{ duration: 0.18, ease: 'easeOut' }}
            >
              {page === 'drive' && <DrivePage />}
              {page === 'scope' && <ScopePage />}
              {page === 'tests' && <TestsPage />}
              {page === 'params' && <ParamsPage />}
              {page === 'console' && <ConsolePage />}
            </motion.div>
          </AnimatePresence>
        </main>
        <footer className="mt-20 bg-slate text-ivory">
          <div className="flex flex-wrap items-center justify-between gap-4 px-4 py-6 sm:px-6 2xl:px-10">
            <span className="font-sans text-[12px] font-bold tracking-[0.18em] uppercase">BLDC · FOC</span>
            <span className="font-sans text-xs text-cloud">STM32G431 + DRV8316T · board v7 · test_08_drive_lab protocol v1</span>
          </div>
        </footer>
      </div>
      <Toaster position="bottom-right" />
    </TooltipProvider>
  )
}
