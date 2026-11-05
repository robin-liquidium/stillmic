// mictest <seconds> <out prefix>: records Stillmic and the built-in mic at once, as raw float32.
import CoreAudio
import Foundation
func addr(_ s: AudioObjectPropertySelector) -> AudioObjectPropertyAddress { AudioObjectPropertyAddress(mSelector: s, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain) }
func dev(_ uid: String) -> AudioObjectID { var a = addr(kAudioHardwarePropertyTranslateUIDToDevice); var u = uid as CFString; var id = AudioObjectID(0); var n = UInt32(4)
  _ = withUnsafePointer(to: &u) { AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a, UInt32(MemoryLayout<CFString>.size), $0, &n, &id) }; return id }
final class Rec { var b = [Float](); let ch: Int; init(ch: Int) { self.ch = ch; b.reserveCapacity(48000 * 120) } }
let secs = Double(CommandLine.arguments[1])!; let prefix = CommandLine.arguments[2]
var running: [(AudioObjectID, AudioDeviceIOProcID, Rec, String)] = []
for (name, uid, ch) in [("stillmic", "Stillmic:visible:48k2ch", 2), ("raw", "BuiltInMicrophoneDevice", 1)] {
  let d = dev(uid); let r = Rec(ch: ch); var pid: AudioDeviceIOProcID?
  let ctx = Unmanaged.passRetained(r).toOpaque()
  let st = AudioDeviceCreateIOProcID(d, { _, _, inp, _, _, _, c in
    let r = Unmanaged<Rec>.fromOpaque(c!).takeUnretainedValue(); let bl = inp.pointee.mBuffers
    let n = Int(bl.mDataByteSize) / 4 / r.ch; let p = bl.mData!.assumingMemoryBound(to: Float.self)
    for i in 0..<n { r.b.append(p[i * r.ch]) }; return noErr }, ctx, &pid)
  precondition(st == noErr && pid != nil, "ioproc \(name) \(st)")
  AudioDeviceStart(d, pid); running.append((d, pid!, r, name))
}
Thread.sleep(forTimeInterval: secs)
for (d, p, r, name) in running { AudioDeviceStop(d, p); AudioDeviceDestroyIOProcID(d, p)
  r.b.withUnsafeBytes { FileManager.default.createFile(atPath: "\(prefix)_\(name).f32", contents: Data($0)) }; print(name, r.b.count) }
