import CoreAudio
import Foundation
func addr(_ s: AudioObjectPropertySelector) -> AudioObjectPropertyAddress { AudioObjectPropertyAddress(mSelector: s, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain) }
func u32(_ id: AudioObjectID, _ s: AudioObjectPropertySelector) -> UInt32 { var a = addr(s); var v: UInt32 = 0; var n = UInt32(4); AudioObjectGetPropertyData(id, &a, 0, nil, &n, &v); return v }
func i32(_ id: AudioObjectID, _ s: AudioObjectPropertySelector) -> Int32 { var a = addr(s); var v: Int32 = 0; var n = UInt32(4); AudioObjectGetPropertyData(id, &a, 0, nil, &n, &v); return v }
func cf(_ id: AudioObjectID, _ s: AudioObjectPropertySelector) -> String { var a = addr(s); var v: Unmanaged<CFString>?; var n = UInt32(MemoryLayout<Unmanaged<CFString>?>.size); guard AudioObjectGetPropertyData(id, &a, 0, nil, &n, &v) == 0, let r = v else { return "?" }; return r.takeRetainedValue() as String }
let sys = AudioObjectID(kAudioObjectSystemObject)
var a = addr(kAudioHardwarePropertyProcessObjectList); var n: UInt32 = 0
AudioObjectGetPropertyDataSize(sys, &a, 0, nil, &n)
var ids = [AudioObjectID](repeating: 0, count: Int(n) / 4)
AudioObjectGetPropertyData(sys, &a, 0, nil, &n, &ids)
for id in ids {
    let i = u32(id, kAudioProcessPropertyIsRunningInput), o = u32(id, kAudioProcessPropertyIsRunningOutput)
    if i == 0 && o == 0 { continue }
    var da = addr(kAudioProcessPropertyDevices); da.mScope = kAudioObjectPropertyScopeInput; var m: UInt32 = 0
    AudioObjectGetPropertyDataSize(id, &da, 0, nil, &m)
    var devs = [AudioObjectID](repeating: 0, count: Int(m) / 4)
    if m > 0 { AudioObjectGetPropertyData(id, &da, 0, nil, &m, &devs) }
    print("pid \(i32(id, kAudioProcessPropertyPID)) \(cf(id, kAudioProcessPropertyBundleID)) input=\(i) output=\(o) inputDevices=\(devs.map { cf($0, kAudioObjectPropertyName) })")
}
