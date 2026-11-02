import CoreAudio
import Foundation

public struct InputDevice: Hashable {
    public let id: AudioDeviceID
    public let name: String
    public let uid: String
    public let sampleRate: Double
    public let inputChannels: Int
}

private func address(_ selector: AudioObjectPropertySelector,
                     _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal) -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: kAudioObjectPropertyElementMain)
}

private func stringProperty(_ id: AudioObjectID, _ selector: AudioObjectPropertySelector) -> String? {
    var addr = address(selector)
    var value: Unmanaged<CFString>?
    var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &value) == noErr, let v = value else { return nil }
    return v.takeRetainedValue() as String
}

private func inputChannelCount(_ id: AudioDeviceID) -> Int {
    var addr = address(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(id, &addr, 0, nil, &size) == noErr, size > 0 else { return 0 }
    let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: MemoryLayout<AudioBufferList>.alignment)
    defer { raw.deallocate() }
    guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, raw) == noErr else { return 0 }
    let abl = UnsafeMutableAudioBufferListPointer(raw.assumingMemoryBound(to: AudioBufferList.self))
    return abl.reduce(0) { $0 + Int($1.mNumberChannels) }
}

private func nominalSampleRate(_ id: AudioDeviceID) -> Double {
    var addr = address(kAudioDevicePropertyNominalSampleRate)
    var rate: Float64 = 0
    var size = UInt32(MemoryLayout<Float64>.size)
    return AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &rate) == noErr ? rate : 0
}

public func listInputDevices() -> [InputDevice] {
    var addr = address(kAudioHardwarePropertyDevices)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size) == noErr else {
        return []
    }
    var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
    guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &ids) == noErr else {
        return []
    }
    return ids.compactMap { id in
        let channels = inputChannelCount(id)
        guard channels > 0 else { return nil }
        return InputDevice(id: id, name: stringProperty(id, kAudioObjectPropertyName) ?? "?",
                           uid: stringProperty(id, kAudioDevicePropertyDeviceUID) ?? "?",
                           sampleRate: nominalSampleRate(id), inputChannels: channels)
    }
}

public enum DeviceLookup {
    case found(InputDevice)
    case notFound
    case ambiguous([InputDevice])
}

/// Exact UID match first, then case-insensitive name substring.
public func findInputDevice(_ query: String) -> DeviceLookup {
    let devices = listInputDevices()
    if let d = devices.first(where: { $0.uid == query }) { return .found(d) }
    let matches = devices.filter { $0.name.localizedCaseInsensitiveContains(query) }
    if let exact = matches.first(where: { $0.name.caseInsensitiveCompare(query) == .orderedSame }) { return .found(exact) }
    switch matches.count {
    case 0: return .notFound
    case 1: return .found(matches[0])
    default: return .ambiguous(matches)
    }
}
