const std = @import("std");
const builtin = @import("builtin");
const zstd = std.compress.zstd;

const BUNLE_RESOURCES_REL_PATH = "../Resources/";

const AppMetadata = struct {
    identifier: []const u8,
    name: []const u8,
    channel: []const u8,
    hash: ?[]const u8 = null,
};

const UserConfig = struct {
    install_path: []const u8,
    use_discord: bool,
    use_local: bool,
};

pub fn pipeToFileSystem(dir: std.fs.Dir, reader: anytype) !void {
    var file_name_buffer: [255]u8 = undefined;
    var buffer: [512 * 8]u8 = undefined;
    var start: usize = 0;
    var end: usize = 0;
    header: while (true) {
        if (buffer.len - start < 1024) {
            const dest_end = end - start;
            @memcpy(buffer[0..dest_end], buffer[start..end]);
            end = dest_end;
            start = 0;
        }
        const ask_header = @min(buffer.len - end, 1024 -| (end - start));
        end += try reader.readAtLeast(buffer[end..], ask_header);
        switch (end - start) {
            0 => return,
            1...511 => return error.UnexpectedEndOfStream,
            else => {},
        }
        const header: Header = .{ .bytes = buffer[start..][0..512] };
        start += 512;
        const file_size = try header.fileSize();
        const rounded_file_size = std.mem.alignForward(u64, file_size, 512);
        const pad_len = @as(usize, @intCast(rounded_file_size - file_size));
        const unstripped_file_name = try header.fullFileName(&file_name_buffer);
        switch (header.fileType()) {
            .directory => {
                const file_name = unstripped_file_name;
                if (file_name.len != 0) {
                    dir.makePath(file_name) catch |err| {
                        return err;
                    };
                }
            },
            .normal => {
                if (file_size == 0 and unstripped_file_name.len == 0) return;
                const file_name = unstripped_file_name;

                if (std.fs.path.dirname(file_name)) |dir_name| {
                    dir.makePath(dir_name) catch |err| {
                        return err;
                    };
                }

                const mode = if (builtin.os.tag == .windows) 0 else header.mode() catch undefined;
                var file = dir.createFile(file_name, .{ .mode = mode }) catch |err| {
                    return err;
                };
                defer file.close();

                var file_off: usize = 0;
                while (true) {
                    if (buffer.len - start < 1024) {
                        const dest_end = end - start;
                        @memcpy(buffer[0..dest_end], buffer[start..end]);
                        end = dest_end;
                        start = 0;
                    }
                    const ask = @as(usize, @intCast(@min(
                        buffer.len - end,
                        rounded_file_size + 512 - file_off -| (end - start),
                    )));
                    end += try reader.readAtLeast(buffer[end..], ask);
                    if (end - start < ask) return error.UnexpectedEndOfStream;
                    const slice = buffer[start..@as(usize, @intCast(@min(file_size - file_off + start, end)))];
                    try file.writeAll(slice);
                    file_off += slice.len;
                    start += slice.len;
                    if (file_off >= file_size) {
                        start += pad_len;
                        std.debug.assert(start <= end);
                        continue :header;
                    }
                }
            },
            .global_extended_header, .extended_header => {
                if (start + rounded_file_size > end) return error.TarHeadersTooBig;
                start = @as(usize, @intCast(start + rounded_file_size));
            },
            .symbolic_link => {
                if (file_size == 0 and unstripped_file_name.len == 0) return;
                const link_name = unstripped_file_name;

                var link_target_buffer: [1024]u8 = undefined;
                const bytes_to_read = @min(file_size, link_target_buffer.len);
                if (bytes_to_read > 0) {
                    while (end - start < bytes_to_read) {
                        const dest_end = end - start;
                        @memcpy(buffer[0..dest_end], buffer[start..end]);
                        end = dest_end;
                        start = 0;
                        const ask = @min(buffer.len - end, 512);
                        end += try reader.readAtLeast(buffer[end..], ask);
                    }

                    @memcpy(link_target_buffer[0..bytes_to_read], buffer[start .. start + bytes_to_read]);
                    start += file_size;

                    const rounded_link_size = std.mem.alignForward(u64, file_size, 512);
                    const link_pad_len = @as(usize, @intCast(rounded_link_size - file_size));
                    start += link_pad_len;
                    const link_target = link_target_buffer[0..bytes_to_read];

                    if (std.fs.path.dirname(link_name)) |dir_name| {
                        try dir.makePath(dir_name);
                    }

                    if (builtin.os.tag == .windows) {
                        // Skip symlinks on Windows
                    } else {
                        dir.symLink(link_target, link_name, .{}) catch {
                            dir.deleteFile(link_name) catch {};
                            dir.symLink(link_target, link_name, .{}) catch {};
                        };
                    }
                }
            },
            .hard_link => return error.TarUnsupportedFileType,
            else => return error.TarUnsupportedFileType,
        }
    }
}

pub const Header = struct {
    bytes: *const [512]u8,

    pub const FileType = enum(u8) {
        normal = '0',
        hard_link = '1',
        symbolic_link = '2',
        character_special = '3',
        block_special = '4',
        directory = '5',
        fifo = '6',
        contiguous = '7',
        global_extended_header = 'g',
        extended_header = 'x',
        _,
    };

    pub fn fileSize(header: Header) !u64 {
        const raw = header.bytes[124..][0..12];
        const ltrimmed = std.mem.trimLeft(u8, raw, "0");
        const rtrimmed = std.mem.trimRight(u8, ltrimmed, " \x00");
        if (rtrimmed.len == 0) return 0;
        return std.fmt.parseInt(u64, rtrimmed, 8);
    }

    pub fn is_ustar(header: Header) bool {
        return std.mem.eql(u8, header.bytes[257..][0..6], "ustar\x00");
    }

    pub fn fullFileName(header: Header, buffer: *[255]u8) ![]const u8 {
        const n = name(header);
        const result = blk: {
            if (!is_ustar(header))
                break :blk n;
            const p = prefix(header);
            if (p.len == 0)
                break :blk n;
            @memcpy(buffer[0..p.len], p);
            buffer[p.len] = '/';
            @memcpy(buffer[p.len + 1 ..][0..n.len], n);
            break :blk buffer[0 .. p.len + 1 + n.len];
        };
        if (result.len > 0 and (result[0] == '/' or result[0] == '\\')) {
            return error.PathTraversal;
        }
        if (result.len >= 2 and result[1] == ':') {
            return error.PathTraversal;
        }

        var i: usize = 0;
        while (i < result.len) {
            var j = i;
            while (j < result.len and result[j] != '/' and result[j] != '\\') {
                j += 1;
            }
            const component = result[i..j];
            if (std.mem.eql(u8, component, "..")) {
                return error.PathTraversal;
            }
            i = if (j < result.len) j + 1 else j;
        }

        return result;
    }

    pub fn mode(header: Header) !u32 {
        const raw = header.bytes[100..][0..8];
        const ltrimmed = std.mem.trimLeft(u8, raw, "0");
        const rtrimmed = std.mem.trimRight(u8, ltrimmed, " \x00");
        if (rtrimmed.len == 0) return 0;
        return std.fmt.parseInt(u32, rtrimmed, 8);
    }

    pub fn name(header: Header) []const u8 {
        return str(header, 0, 0 + 100);
    }

    pub fn prefix(header: Header) []const u8 {
        return str(header, 345, 345 + 155);
    }

    pub fn fileType(header: Header) FileType {
        const result = @as(FileType, @enumFromInt(header.bytes[156]));
        return if (result == @as(FileType, @enumFromInt(0))) .normal else result;
    }

    fn str(header: Header, start: usize, end: usize) []const u8 {
        var i: usize = start;
        while (i < end) : (i += 1) {
            if (header.bytes[i] == 0) break;
        }
        return header.bytes[start..i];
    }
};

// --- PAYLOAD RETRIEVAL HELPER ---

/// Reads the appended .tar.zst payload from the end of the current executable.
/// Expects an 8-byte little-endian footer indicating the payload size.
fn getAppendedPayload(allocator: std.mem.Allocator) ![]u8 {
    const exe_path = try std.fs.selfExePathAlloc(allocator);
    defer allocator.free(exe_path);

    const file = try std.fs.openFileAbsolute(exe_path, .{});
    defer file.close();

    const file_size = try file.getEndPos();
    if (file_size < 8) return error.InvalidExecutable;

    // Read the last 8 bytes to get the payload size
    try file.seekTo(file_size - 8);
    var size_buf: [8]u8 = undefined;
    _ = try file.readAll(&size_buf);
    const payload_size = std.mem.readInt(u64, &size_buf, .little);

    if (payload_size == 0 or payload_size > file_size) return error.InvalidPayloadSize;

    // Calculate start position and read the payload
    const payload_start = file_size - 8 - payload_size;
    try file.seekTo(payload_start);

    const payload = try allocator.alloc(u8, payload_size);
    errdefer allocator.free(payload);
    _ = try file.readAll(payload);

    return payload;
}

// --- SETUP HELPERS ---

fn askUser(allocator: std.mem.Allocator, prompt: []const u8, default_val: []const u8) ![]const u8 {
    const stdout = std.io.getStdOut().writer();
    const stdin = std.io.getStdIn().reader();
    try stdout.print("{s} [{s}]: ", .{ prompt, default_val });
    var buffer: [1024]u8 = undefined;
    if (try stdin.readUntilDelimiterOrEof(&buffer, '\n')) |line| {
        const trimmed = std.mem.trim(u8, line, "\r ");
        if (trimmed.len > 0) return allocator.dupe(u8, trimmed);
    }
    return allocator.dupe(u8, default_val);
}

fn askYesNo(allocator: std.mem.Allocator, prompt: []const u8) !bool {
    const answer = try askUser(allocator, prompt, "y");
    defer allocator.free(answer);
    return (std.ascii.eqlIgnoreCase(answer, "y") or std.ascii.eqlIgnoreCase(answer, "yes"));
}

fn gatherUserPreferences(allocator: std.mem.Allocator, default_base_dir: []const u8, metadata: AppMetadata) !UserConfig {
    const stdout = std.io.getStdOut().writer();
    try stdout.print("\n=== {s} Setup ===\n", .{metadata.name});
    const chosen_path = try askUser(allocator, "Enter installation path", default_base_dir);
    const use_discord = try askYesNo(allocator, "Enable Discord Rich Presence? (y/n)");
    const use_local = try askYesNo(allocator, "Enable Local File Mode? (y/n)");
    return UserConfig{ .install_path = chosen_path, .use_discord = use_discord, .use_local = use_local };
}

fn writeCustomConfigs(allocator: std.mem.Allocator, app_dir: []const u8, config: UserConfig) !void {
    var dir = try std.fs.cwd().openDir(app_dir, .{});
    defer dir.close();
    const random = std.crypto.random;
    const port1 = random.intRangeAtMost(u16, 10000, 12000);
    var port2 = random.intRangeAtMost(u16, 10000, 12000);
    while (port2 == port1) port2 = random.intRangeAtMost(u16, 10000, 12000);

    const json_path = "Resources/app/data/system.json";
    var json_arena = std.heap.ArenaAllocator.init(allocator);
    defer json_arena.deinit();
    const json_alloc = json_arena.allocator();

    var root_value: std.json.Value = .null;
    if (dir.openFile(json_path, .{})) |file| {
        defer file.close();
        if (file.readToEndAlloc(json_alloc, 1024 * 1024)) |content| {
            if (content.len > 0) {
                if (std.json.parseFromSlice(std.json.Value, json_alloc, content, .{})) |parsed| {
                    root_value = parsed.value;
                } else |_| {}
            }
        } else |_| {}
    } else |_| {}

    if (root_value != .object) root_value = std.json.Value{ .object = std.json.ObjectMap.init(json_alloc) };
    try root_value.object.put("isDiscord", .{ .bool = config.use_discord });
    try root_value.object.put("isLocal", .{ .bool = config.use_local });
    try root_value.object.put("appPort", .{ .integer = port1 });
    try root_value.object.put("playerPort", .{ .integer = port2 });
    if (config.use_discord) try root_value.object.put("DiscordClientId", .{ .string = "1456480026869629094" });

    if (std.fs.path.dirname(json_path)) |parent| dir.makePath(parent) catch {};
    const json_file = try dir.createFile(json_path, .{});
    defer json_file.close();
    try std.json.stringify(root_value, .{ .whitespace = .indent_2 }, json_file.writer());
}

// --- PROGRESS & EXTRACTION LOGIC ---

const ProgressIndicator = struct {
    child_process: ?std.process.Child,
    allocator: std.mem.Allocator,
    spinner_thread: ?std.Thread = null,
    should_stop: std.atomic.Value(bool) = std.atomic.Value(bool).init(false),
    app_name: []const u8 = "",

    fn init(allocator: std.mem.Allocator, metadata: AppMetadata) ProgressIndicator {
        // Just initialize and return the struct. Do not spawn the thread yet.
        return ProgressIndicator{ .child_process = null, .allocator = allocator, .app_name = metadata.name };
    }

    fn spinnerThread(self: *ProgressIndicator) void {
        const chars = [_]u8{ '|', '/', '-', '\\' };
        var frame: usize = 0;
        std.debug.print("Installing {s}... ", .{self.app_name});
        while (!self.should_stop.load(.acquire)) {
            std.debug.print("{c}\x08", .{chars[frame]});
            frame = (frame + 1) % chars.len;
            std.time.sleep(100 * std.time.ns_per_ms);
        }
        std.debug.print("Done!\n", .{});
    }

    fn startProgressDialog(self: *ProgressIndicator) !void {
        if (builtin.os.tag == .windows) {
            self.spinner_thread = try std.Thread.spawn(.{}, spinnerThread, .{self});
        }
    }

    fn deinit(self: *ProgressIndicator) void {
        if (self.spinner_thread) |thread| {
            self.should_stop.store(true, .release);
            thread.join();
        }
    }
};

fn extractFromSelf(allocator: std.mem.Allocator) !bool {
    std.debug.print("Starting standalone Kuumo Setup...\n", .{});

    // 1. Dynamic Retrieval of payload
    const archive_data = try getAppendedPayload(allocator);
    defer allocator.free(archive_data);

    const safe_metadata = AppMetadata{
        .identifier = "com.kuumo.app",
        .name = "Kuumo App",
        .channel = "stable",
        .hash = "embedded-release",
    };

    const default_app_data_dir = try getAppDataDir(allocator);
    defer allocator.free(default_app_data_dir);
    const default_app_base_dir = try std.fs.path.join(allocator, &.{ default_app_data_dir, safe_metadata.identifier, safe_metadata.channel });
    defer allocator.free(default_app_base_dir);

    const config = try gatherUserPreferences(allocator, default_app_base_dir, safe_metadata);
    const self_extraction_dir = try std.fs.path.join(allocator, &.{ config.install_path, "self-extraction" });
    defer allocator.free(self_extraction_dir);
    const app_dir = try std.fs.path.join(allocator, &.{ config.install_path, "Kuumo app" });
    defer allocator.free(app_dir);

    return try extractAndInstall(allocator, archive_data, safe_metadata, self_extraction_dir, app_dir, config);
}

fn extractAndInstall(allocator: std.mem.Allocator, compressed_data: []const u8, metadata: AppMetadata, self_extraction_dir: []const u8, app_dir: []const u8, config: UserConfig) !bool {
    // 1. Create the struct on the stack
    var progress = ProgressIndicator.init(allocator, metadata);

    // 2. Start the thread using the stable pointer
    progress.startProgressDialog() catch std.debug.print("Installing {s}...\n", .{metadata.name});

    // 3. Ensure it cleans up before this function exits
    defer progress.deinit();

    const window_buffer = try allocator.alloc(u8, 128 * 1024 * 1024);
    defer allocator.free(window_buffer);

    var stream = std.io.fixedBufferStream(compressed_data);
    var decompressor = zstd.decompressor(stream.reader(), .{ .window_buffer = window_buffer });
    var decompressed_data = std.ArrayList(u8).init(allocator);
    defer decompressed_data.deinit();

    var buffer: [4096]u8 = undefined;
    while (true) {
        const read_size = try decompressor.reader().read(&buffer);
        if (read_size == 0) break;
        try decompressed_data.appendSlice(buffer[0..read_size]);
    }

    try extractTar(allocator, decompressed_data.items, self_extraction_dir);

    const sanitized_name = try std.mem.replaceOwned(u8, allocator, metadata.name, " ", "");
    defer allocator.free(sanitized_name);
    const dots_removed = try std.mem.replaceOwned(u8, allocator, sanitized_name, ".", "-");
    defer allocator.free(dots_removed);

    const extracted_app_path = try std.fs.path.join(allocator, &.{ self_extraction_dir, dots_removed });
    defer allocator.free(extracted_app_path);

    if (builtin.os.tag == .windows) {
        try std.fs.cwd().makePath(app_dir);
        try copyDirectory(allocator, extracted_app_path, app_dir);
        try writeCustomConfigs(allocator, app_dir, config);
    }

    std.debug.print("Installation completed successfully!\n", .{});
    return true;
}

// --- SYSTEM UTILS (Clipped for brevity, identical to your original source) ---

fn getAppDataDir(allocator: std.mem.Allocator) ![]const u8 {
    return switch (builtin.os.tag) {
        .windows => blk: {
            const local = std.process.getEnvVarOwned(allocator, "LOCALAPPDATA") catch try std.process.getEnvVarOwned(allocator, "APPDATA");
            break :blk local;
        },
        else => @compileError("Unsupported platform"),
    };
}

fn extractTar(allocator: std.mem.Allocator, tar_data: []const u8, extract_dir: []const u8) !void {
    _ = allocator;
    try std.fs.cwd().makePath(extract_dir);
    const dir = try std.fs.cwd().openDir(extract_dir, .{});
    var stream = std.io.fixedBufferStream(tar_data);
    try pipeToFileSystem(dir, stream.reader());
}

fn copyDirectory(allocator: std.mem.Allocator, src_path: []const u8, dest_path: []const u8) !void {
    var src_dir = try std.fs.cwd().openDir(src_path, .{ .iterate = true });
    defer src_dir.close();
    var iterator = src_dir.iterate();
    while (try iterator.next()) |entry| {
        const src_item = try std.fs.path.join(allocator, &.{ src_path, entry.name });
        defer allocator.free(src_item);
        const dest_item = try std.fs.path.join(allocator, &.{ dest_path, entry.name });
        defer allocator.free(dest_item);
        if (entry.kind == .directory) {
            try std.fs.cwd().makePath(dest_item);
            try copyDirectory(allocator, src_item, dest_item);
        } else {
            try std.fs.cwd().copyFile(src_item, std.fs.cwd(), dest_item, .{});
        }
    }
}

// --- HEADER & PIPE UTILS (Keep your original implementations) ---
// [Include your pipeToFileSystem, Header struct, etc. from original code]

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    if (builtin.os.tag == .windows or builtin.os.tag == .linux) {
        _ = try extractFromSelf(allocator);
    }
}
