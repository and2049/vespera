import AppKit
import SwiftUI

@MainActor
struct CarouselNavigation {
  let count: Int
  let selectedIndex: Int
  let onSelect: (Int) -> Void

  func step(_ direction: Int) {
    guard count > 0 else { return }
    select(((selectedIndex + direction) % count + count) % count)
  }

  func select(_ index: Int) {
    guard (0..<count).contains(index) else { return }
    withAnimation(.easeInOut(duration: 0.18)) { onSelect(index) }
  }
}

struct CarouselSwipe {
  enum State {
    case idle
    case tracking(Double)
    case selected
  }
  private(set) var state = State.idle

  mutating func reset() { state = .idle }

  mutating func consume(_ delta: Double) -> Int? {
    let distance: Double
    switch state {
    case .idle: distance = delta
    case .tracking(let accumulated): distance = accumulated + delta
    case .selected: return nil
    }
    if abs(distance) >= 35 {
      state = .selected
      return distance < 0 ? 1 : -1
    }
    state = .tracking(distance)
    return nil
  }
}

struct CarouselScrollInput: NSViewRepresentable {
  let onStep: (Int) -> Void

  func makeNSView(context: Context) -> ScrollView { ScrollView(onStep: onStep) }
  func updateNSView(_ view: ScrollView, context: Context) { view.onStep = onStep }
  static func dismantleNSView(_ view: ScrollView, coordinator: ()) { view.stop() }

  final class ScrollView: NSView {
    var onStep: (Int) -> Void
    private var monitor: Any?
    private var swipe = CarouselSwipe()
    private var lastEventTime = 0.0

    init(onStep: @escaping (Int) -> Void) {
      self.onStep = onStep
      super.init(frame: .zero)
      monitor = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { [weak self] event in
        guard let self, let window = self.window, event.window === window,
          !self.isHiddenOrHasHiddenAncestor,
          self.bounds.contains(self.convert(event.locationInWindow, from: nil))
        else { return event }
        return self.handle(event)
      }
    }

    required init?(coder: NSCoder) { nil }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }

    func stop() {
      if let monitor { NSEvent.removeMonitor(monitor) }
      monitor = nil
    }

    private func handle(_ event: NSEvent) -> NSEvent? {
      guard event.momentumPhase.isEmpty else { return event }
      if event.phase.contains(.began) || event.timestamp - lastEventTime > 0.25 { swipe.reset() }
      lastEventTime = event.timestamp
      if event.phase.contains(.ended) || event.phase.contains(.cancelled) {
        swipe.reset()
        return event
      }
      guard abs(event.scrollingDeltaX) > abs(event.scrollingDeltaY) else { return event }
      let delta = Double(event.scrollingDeltaX) * (event.hasPreciseScrollingDeltas ? 1 : 12)
      if let direction = swipe.consume(delta) { onStep(direction) }
      return nil
    }
  }
}

struct ModuleCarousel: View {
  static let iconSide: CGFloat = 48

  let modules: [LoadedModule]
  let selectedIndex: Int
  let label: String
  var enabled = true
  var capturesScroll = true
  var width: CGFloat = 536
  /// IDs of modules marked with a dot beneath their icon, such as those processing.
  var marked: Set<String> = []
  let onSelect: (Int) -> Void
  @State private var hoveredID: String?

  private func offset(for index: Int) -> Int {
    let count = modules.count
    guard count > 0 else { return 0 }
    let distance = (index - selectedIndex + count) % count
    return distance > count / 2 ? distance - count : distance
  }

  private func step(_ direction: Int) {
    guard enabled else { return }
    navigation.step(direction)
  }

  private func select(_ index: Int) {
    guard enabled else { return }
    navigation.select(index)
  }

  private var navigation: CarouselNavigation {
    CarouselNavigation(count: modules.count, selectedIndex: selectedIndex, onSelect: onSelect)
  }

  var body: some View {
    ZStack {
      ForEach(modules.indices, id: \.self) { index in
        let offset = offset(for: index)
        let module = modules[index]
        Button {
          select(index)
        } label: {
          Image(nsImage: module.icon)
            .resizable().scaledToFit()
            .frame(width: Self.iconSide, height: Self.iconSide)
            .opacity(offset == 0 || hoveredID == module.id ? 1 : abs(offset) == 1 ? 0.6 : 0.4)
            .frame(width: 96 * min(1, width / 536), height: 80)
            .overlay(alignment: .bottom) {
              if marked.contains(module.id) {
                Circle().fill(Theme.fill)
                  .frame(
                    width: ModuleTabButton.indicatorSize, height: ModuleTabButton.indicatorSize
                  )
                  .padding(.bottom, 6)
                  .accessibilityHidden(true)
              }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .onHover { hoveredID = $0 ? module.id : nil }
        .help([module.pluginFamily, module.name].compactMap { $0 }.joined(separator: " — "))
        .accessibilityLabel(module.name)
        .accessibilityValue(marked.contains(module.id) ? "In signal chain" : "")
        .accessibilityAddTraits(offset == 0 ? [.isSelected] : [])
        .offset(x: CGFloat(offset) * 110 * min(1, width / 536))
        .opacity(abs(offset) <= 2 ? 1 : 0)
        .allowsHitTesting(abs(offset) <= 2)
        .accessibilityHidden(abs(offset) > 2)
      }
    }
    .frame(width: width, height: 80)
    .padding(.bottom, 4)
    .background {
      if capturesScroll { CarouselScrollInput(onStep: step) }
    }
    .accessibilityElement(children: .contain)
    .accessibilityLabel(label)
    .accessibilityValue(modules.indices.contains(selectedIndex) ? modules[selectedIndex].name : "")
    .accessibilityAdjustableAction { direction in
      switch direction {
      case .increment: step(1)
      case .decrement: step(-1)
      @unknown default: break
      }
    }
  }
}
