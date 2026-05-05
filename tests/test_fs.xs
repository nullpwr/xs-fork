-- filesystem module tests
import fs
import os
import time

let is_windows = os.platform == "windows"

-- write and read
let tmp = fs.temp_file()
fs.write(tmp, "hello world")
assert_eq(fs.read(tmp), "hello world")

-- exists, is_file, is_dir
assert_eq(fs.exists(tmp), true)
assert_eq(fs.is_file(tmp), true)
assert_eq(fs.is_dir(tmp), false)

-- size
assert_eq(fs.size(tmp), 11)

-- stat
let info = fs.stat(tmp)
assert_eq(info.size, 11)
assert_eq(info.is_file, true)
assert_eq(info.is_dir, false)

-- append
fs.append(tmp, "!")
assert_eq(fs.read(tmp), "hello world!")

-- copy
let tmp2 = tmp + ".copy"
fs.copy(tmp, tmp2)
assert_eq(fs.read(tmp2), "hello world!")
fs.remove(tmp2)

-- rename
let tmp3 = tmp + ".renamed"
fs.rename(tmp, tmp3)
assert_eq(fs.exists(tmp), false)
assert_eq(fs.read(tmp3), "hello world!")
fs.remove(tmp3)

-- basename, dirname, ext
assert_eq(fs.basename("/foo/bar/baz.txt"), "baz.txt")
assert_eq(fs.dirname("/foo/bar/baz.txt"), "/foo/bar")
assert_eq(fs.ext("/foo/bar/baz.txt"), ".txt")

-- join
assert_eq(fs.join("foo", "bar"), "foo/bar")
assert_eq(fs.join("/root", "sub", "file.xs"), "/root/sub/file.xs")

-- mkdir_p and rmdir
let test_dir = fs.temp_dir() + "/xs_test_dir_" + str(time.now())
fs.mkdir_p(test_dir)
assert_eq(fs.is_dir(test_dir), true)
fs.rmdir(test_dir)

-- ls
let ls_dir = fs.temp_dir() + "/xs_ls_test_" + str(time.now())
fs.mkdir_p(ls_dir)
fs.write(ls_dir + "/a.txt", "a")
fs.write(ls_dir + "/b.txt", "b")
let entries = fs.ls(ls_dir)
assert(entries.len() >= 2)
fs.remove(ls_dir + "/a.txt")
fs.remove(ls_dir + "/b.txt")
fs.rmdir(ls_dir)

-- temp_dir returns a string
assert(fs.temp_dir().len() > 0)

-- abs returns absolute path
let abs_path = fs.abs(".")
if is_windows {
    assert(abs_path.len() >= 3)
} else {
    assert(abs_path.starts_with("/"))
}

-- read_bytes / write_bytes
let btmp = fs.temp_file()
fs.write_bytes(btmp, [72, 73])
let bytes = fs.read_bytes(btmp)
assert_eq(bytes[0], 72)
assert_eq(bytes[1], 73)
fs.remove(btmp)

-- read_lines
let rl_tmp = fs.temp_file()
fs.write(rl_tmp, "line1\nline2\nline3\n")
let lines = fs.read_lines(rl_tmp)
assert_eq(lines.len(), 3)
assert_eq(lines[0], "line1")
assert_eq(lines[1], "line2")
assert_eq(lines[2], "line3")
fs.remove(rl_tmp)

-- read_stream / write_stream
let ws_tmp = fs.temp_file()
let writer = fs.write_stream(ws_tmp)
writer.write(writer, "hello ")
writer.write(writer, "stream")
writer.close(writer)
let reader = fs.read_stream(ws_tmp)
let content = reader.read_all(reader)
assert_eq(content, "hello stream")
reader.close(reader)
fs.remove(ws_tmp)

-- read_stream line by line
let rs_tmp = fs.temp_file()
fs.write(rs_tmp, "alpha\nbeta\ngamma\n")
let r2 = fs.read_stream(rs_tmp)
let l1 = r2.read_line(r2)
let l2 = r2.read_line(r2)
let l3 = r2.read_line(r2)
assert_eq(l1, "alpha")
assert_eq(l2, "beta")
assert_eq(l3, "gamma")
r2.close(r2)
fs.remove(rs_tmp)

-- walk
let walk_dir = fs.temp_dir() + "/xs_walk_test_" + str(time.now())
fs.mkdir_p(walk_dir)
fs.mkdir_p(walk_dir + "/sub")
fs.write(walk_dir + "/top.txt", "top")
fs.write(walk_dir + "/sub/inner.txt", "inner")
let walked = fs.walk(walk_dir)
assert(walked.len() >= 3)
var found_inner = false
for entry in walked {
    if entry.name == "inner.txt" {
        found_inner = true
        assert_eq(entry.is_file, true)
    }
}
assert(found_inner)
fs.remove(walk_dir + "/sub/inner.txt")
fs.remove(walk_dir + "/top.txt")
fs.rmdir(walk_dir + "/sub")
fs.rmdir(walk_dir)

-- glob
let glob_results = fs.glob("tests/test_*.xs")
assert(glob_results.len() >= 10)

-- chmod (unix only, windows has limited permission model)
if !is_windows {
    let ch_tmp = fs.temp_file()
    fs.write(ch_tmp, "test")
    assert_eq(fs.chmod(ch_tmp, 0o644), true)
    fs.remove(ch_tmp)
}

-- symlink / readlink / realpath (unix only, mingw has no symlink support)
if !is_windows {
    let sym_src = fs.temp_file()
    let sym_lnk = sym_src + ".link"
    fs.write(sym_src, "symlink test")
    fs.symlink(sym_src, sym_lnk)
    assert_eq(fs.exists(sym_lnk), true)
    let target = fs.readlink(sym_lnk)
    assert_eq(target, sym_src)
    let real = fs.realpath(sym_lnk)
    assert_eq(real, sym_src)
    fs.remove(sym_lnk)
    fs.remove(sym_src)
}

print("test_fs: all passed")
