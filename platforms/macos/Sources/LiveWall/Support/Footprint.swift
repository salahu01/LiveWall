import Foundation

/// Reads this process's real memory footprint — the same number Activity Monitor
/// shows as "Memory" and `footprint(1)` reports as phys_footprint.
/// Exposed in the status menu so the low-memory claim is verifiable at a glance.
enum Footprint {
    static func bytes() -> UInt64 {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
        let result = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        guard result == KERN_SUCCESS else { return 0 }
        return info.phys_footprint
    }

    static func megabytes() -> Double {
        Double(bytes()) / (1024 * 1024)
    }

    static func formatted() -> String {
        String(format: "%.1f MB", megabytes())
    }
}
