// LövGlöd-skärmsläckare: spelar produktfilmen i loop, utan ljud.
import AVFoundation
import ScreenSaver
import os

private let logg = Logger(subsystem: "lovglod.screensaver", category: "vy")

@objc(LovGlodView)
final class LovGlodView: ScreenSaverView {
    private var player: AVQueuePlayer?
    private var looper: AVPlayerLooper?
    private let playerLayer = AVPlayerLayer()

    override init?(frame: NSRect, isPreview: Bool) {
        super.init(frame: frame, isPreview: isPreview)
        logg.log("init preview=\(isPreview) frame=\(frame.debugDescription, privacy: .public)")
        setUp()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        setUp()
    }

    private func setUp() {
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor

        playerLayer.frame = bounds
        playerLayer.videoGravity = .resizeAspectFill
        playerLayer.autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
        playerLayer.backgroundColor = NSColor.black.cgColor
        layer?.addSublayer(playerLayer)

        let bundle = Bundle(for: LovGlodView.self)
        guard let url = bundle.url(forResource: "produktfilm", withExtension: "mp4") else {
            logg.error("hittar inte produktfilm.mp4 i \(bundle.bundlePath, privacy: .public)")
            return
        }

        let queue = AVQueuePlayer()
        queue.isMuted = true
        queue.preventsDisplaySleepDuringVideoPlayback = false
        looper = AVPlayerLooper(player: queue, templateItem: AVPlayerItem(url: url))
        playerLayer.player = queue
        player = queue

        // Värdprocessen (legacyScreenSaver) anropar inte alltid startAnimation,
        // så börja spela direkt och lita inte på det.
        queue.play()

        // Sedan Sonoma avslutas inte legacyScreenSaver när skärmsläckaren
        // stängs, så pausa själva i stället för att spela vidare i bakgrunden.
        let dnc = DistributedNotificationCenter.default()
        dnc.addObserver(self, selector: #selector(willStop),
                        name: NSNotification.Name("com.apple.screensaver.willstop"), object: nil)
        dnc.addObserver(self, selector: #selector(didStart),
                        name: NSNotification.Name("com.apple.screensaver.didstart"), object: nil)
    }

    // Vyn skapas ofta med storleken 0×0 och ändras sedan; lagrets
    // autoresizing hänger inte med, så sätt ramen själva.
    override func layout() {
        super.layout()
        syncFrame()
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        syncFrame()
    }

    private func syncFrame() {
        logg.log("syncFrame bounds=\(self.bounds.debugDescription, privacy: .public)")
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        playerLayer.frame = bounds
        CATransaction.commit()
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        logg.log("viewDidMoveToWindow window=\(self.window != nil)")
        if window != nil { player?.play() }
    }

    override func startAnimation() {
        super.startAnimation()
        logg.log("startAnimation bounds=\(self.bounds.debugDescription, privacy: .public)")
        player?.play()
    }

    override func stopAnimation() {
        super.stopAnimation()
        logg.log("stopAnimation")
        player?.pause()
    }

    @objc private func didStart() {
        logg.log("didstart")
        syncFrame()
        player?.play()
        for delay in [0.5, 2.0, 5.0] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in self?.report(delay) }
        }
    }

    private func report(_ t: Double) {
        let p = player
        logg.log("""
            t=\(t) bounds=\(self.bounds.debugDescription, privacy: .public) \
            window=\(self.window?.frame.debugDescription ?? "nil", privacy: .public) \
            hidden=\(self.isHiddenOrHasHiddenAncestor) layer=\(self.playerLayer.frame.debugDescription, privacy: .public) \
            superlayer=\(self.playerLayer.superlayer != nil) ready=\(self.playerLayer.isReadyForDisplay) \
            rate=\(p?.rate ?? -1) status=\(p?.currentItem?.status.rawValue ?? -1) \
            err=\(p?.currentItem?.error?.localizedDescription ?? "-", privacy: .public) \
            time=\(p?.currentTime().seconds ?? -1)
            """)
    }

    @objc private func willStop() {
        logg.log("willstop")
        player?.pause()
    }

    override func animateOneFrame() {}

    override var hasConfigureSheet: Bool { false }
}
