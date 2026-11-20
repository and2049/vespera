#include "fixtures/file_config.hpp"

#include <vespera/file_content.hpp>
#include <vespera/host_services.hpp>
#include <vespera/preset.hpp>

#include <bit>
#include <cstdio>
#include <fstream>
#include <unistd.h>

namespace {
using namespace vespera;
namespace fs = std::filesystem;
namespace kit = vespera::kit;
int failures = 0;
void check(bool value, int line) {
  if (!value) {
    std::fprintf(stderr, "files check failed at line %d\n", line);
    ++failures;
  }
}
#define CHECK(value) check(bool(value), __LINE__)

template <class Action> bool rejects(Action action) {
  try {
    action();
  } catch (const std::exception &) {
    return true;
  }
  return false;
}

std::vector<uint8_t> wave(uint32_t bits = 16) {
  std::vector<uint8_t> result;
  const auto word = [&](uint32_t value, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; ++i) {
      result.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
  };
  const auto text = [&](const char *value) { result.insert(result.end(), value, value + 4); };
  text("RIFF");
  word(36 + 16 * (bits / 8), 4);
  text("WAVE");
  text("fmt ");
  word(16, 4);
  word(bits == 32 ? 3 : 1, 2);
  word(1, 2);
  word(48000, 4);
  word(48000 * (bits / 8), 4);
  word(bits / 8, 2);
  word(bits, 2);
  text("data");
  word(16 * (bits / 8), 4);
  word(bits == 32 ? std::bit_cast<uint32_t>(0.5f) : uint32_t{1} << (bits - 2), bits / 8);
  for (size_t i = 0; i < 15; ++i) {
    word(0, bits / 8);
  }
  return result;
}

void write(const fs::path &path, const std::vector<uint8_t> &bytes) {
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

FileReference reference(const fs::path &path, const std::vector<uint8_t> &bytes) {
  return {VS_FILE_PRESENT, path.string(), path.filename().string(), bytes.size(),
          content_sha256(bytes)};
}

std::vector<float> render(Graph &graph, uint32_t partition) {
  std::vector<float> result(2048), input(2048, 1);
  for (size_t position = 0; position < result.size();) {
    const auto frames =
        static_cast<uint32_t>(std::min<size_t>(partition, result.size() - position));
    const float *inputs[] = {input.data() + position};
    float *outputs[] = {result.data() + position};
    CHECK(graph.process({inputs, outputs, 1, frames, position, VS_PROCESS_OFFLINE}) == VS_OK);
    position += frames;
  }
  return result;
}
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 2;
  }
  std::string directory = (fs::temp_directory_path() / "vespera-files-XXXXXX").string();
  if (!::mkdtemp(directory.data())) {
    return 2;
  }
  const fs::path root(directory), path = root / "response.wav";
  struct Cleanup {
    fs::path path;
    ~Cleanup() { fs::remove_all(path); }
  } cleanup{root};
  CHECK(file_hash_text(content_sha256({})) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(file_hash_text(content_sha256({'a', 'b', 'c'})) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string multi = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  CHECK(file_hash_text(content_sha256({multi.begin(), multi.end()})) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(file_hash_text(content_sha256(std::vector<uint8_t>(1000000, 'a'))) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  CHECK(!safe_relative_file_path("../secret"));
  CHECK(!safe_relative_file_path("/secret"));
  CHECK(!safe_relative_file_path("x/../secret"));
  CHECK(!safe_relative_file_path("x//secret"));
  CHECK(!safe_relative_file_path("x/"));
  CHECK(safe_relative_file_path("impulses/response.wav"));
  CHECK(!valid_file_types("wav,,aif"));
  CHECK(!valid_file_types(".wav"));
  auto api = kit::Module<file_fixture::Config>::files_api;
  CHECK(describe_file_slots(&api)->size() == 1);
  api.slot_size = 1;
  CHECK(!describe_file_slots(&api));
  kit::Job job;
  for (auto bits : {16u, 24u, 32u}) {
    const auto bytes = wave(bits);
    write(path, bytes);
    const auto audio = kit::read_audio(reference(path, bytes), job);
    CHECK(audio.sample_rate == 48000 && audio.channels.size() == 1);
    CHECK(audio.channels[0][0] == 0.5f && audio.channels[0][1] == 0);
  }
  const auto bytes = wave();
  write(path, bytes);
  const auto file = reference(path, bytes);
  CHECK(file.key(48000, 128) != file.key(44100, 128));
  CHECK(file.key(48000, 128, "a") != file.key(48000, 128, "b"));
  CHECK(rejects([&] { file.key(0, 128); }));
  fs::create_symlink(path, root / "symlink.wav");
  CHECK(rejects([&] { read_file_content(root / "symlink.wav", max_asset_bytes); }));
  CHECK(rejects([&] { read_file_content(path, 1); }));
  CHECK(rejects([&] { read_file_content(root, max_asset_bytes); }));
  CHECK(::mkfifo((root / "fifo").c_str(), 0600) == 0);
  CHECK(rejects([&] { read_file_content(root / "fifo", max_asset_bytes); }));
  CHECK(rejects([&] { read_file_content(path, max_asset_bytes, [] { return true; }); }));
  auto nul = file;
  nul.path += std::string("\0suffix", 7);
  CHECK(!valid_file_reference(nul, max_asset_bytes));
  CHECK(rejects([&] { read_file_content(nul.path, max_asset_bytes); }));
  std::vector<float> expected;
  for (auto partition : {1u, 7u, 64u, 512u}) {
    Graph graph;
    graph.set_host_services(HostServices::create({HostWorkMode::synchronous}));
    const auto node = graph.add_module(argv[1]);
    const auto before = graph.capture_preset();
    CHECK(graph.nodes()[0].module.file_slots.size() == 1);
    CHECK(rejects([&] { graph.set_file(node, 2, file); }));
    graph.set_file(node, 1, file);
    const auto preset = graph.capture_preset();
    CHECK(before.schema_version == 1 && before.nodes[0].state.bytes == preset.nodes[0].state.bytes);
    CHECK(preset.schema_version == 2 && preset.nodes[0].files.size() == 1);
    const auto text = serialize_preset(preset);
    CHECK(parse_preset(text).nodes[0].files[0].reference.sha256 == file.sha256);
    write_preset(root / "rig.json", preset);
    CHECK(read_preset(root / "rig.json").nodes[0].files[0].reference.path == path.string());
    graph.prepare({48000, 512, 1});
    CHECK(rejects([&] { graph.set_file(node, 1, file); }));
    CHECK(graph.module_status(node)->kind == VS_STATUS_READY);
    const auto output = render(graph, partition);
    CHECK(output.front() == 0 && output.back() == 0.5f);
    if (expected.empty()) {
      expected = output;
    } else {
      CHECK(expected == output);
    }
  }
  for (auto mode : {HostWorkMode::manual, HostWorkMode::synchronous}) {
    auto services = HostServices::create({mode});
    Graph graph;
    graph.set_host_services(services);
    const auto node = graph.add_module(argv[1]);
    graph.set_file(node, 1, file);
    graph.prepare({48000, 512, 1});
    if (mode == HostWorkMode::manual) {
      CHECK(graph.module_status(node)->kind == VS_STATUS_BUSY);
      const auto silence = render(graph, 64);
      CHECK(std::all_of(silence.begin(), silence.end(), [](float v) { return v == 0; }));
      services->run_pending();
    }
    CHECK(graph.module_status(node)->kind == VS_STATUS_READY);
    graph.release();
    CHECK(services->statistics().produced == services->statistics().disposed);
  }
  {
    auto services = HostServices::create({HostWorkMode::manual});
    Graph graph;
    graph.set_host_services(services);
    const auto node = graph.add_module(argv[1]);
    graph.set_file(node, 1, file);
    graph.prepare({48000, 512, 1});
    graph.release();
    services->run_pending();
    CHECK(services->statistics().closed == 1 && services->statistics().runs == 0);
    CHECK(services->statistics().produced == services->statistics().disposed);
  }
  {
    Graph graph;
    const auto node = graph.add_module(argv[1]);
    graph.set_file(node, 1, {VS_FILE_MISSING, "", "unresolved.wav", 100, {}});
    const auto preset = parse_preset(serialize_preset(graph.capture_preset()));
    CHECK(preset.nodes[0].files[0].reference.path.empty());
    CHECK(preset.nodes[0].files[0].reference.name == "unresolved.wav");
  }
  for (const auto &bad :
       {std::vector<uint8_t>{}, std::vector<uint8_t>{1, 2, 3}, std::vector<uint8_t>(40000)}) {
    write(path, bad);
    Graph graph;
    graph.set_host_services(HostServices::create({HostWorkMode::synchronous}));
    const auto node = graph.add_module(argv[1]);
    if (bad.size() <= 32768) {
      graph.set_file(node, 1, reference(path, bad));
    } else {
      CHECK(rejects([&] { graph.set_file(node, 1, reference(path, bad)); }));
      graph.set_file(node, 1, FileReference{VS_FILE_MISSING, "", "response.wav", 100, {}});
    }
    graph.prepare({48000, 512, 1});
    CHECK(graph.module_status(node)->kind == VS_STATUS_ERROR);
    CHECK(graph.module_status(node)->file_slot == 1);
    render(graph, 64);
  }
  write(path, bytes);
  fs::remove(path);
  CHECK(resolve_file_reference(file, 32768).state == VS_FILE_MISSING);
  write(path, {1, 2, 3});
  CHECK(resolve_file_reference(file, 32768).state == VS_FILE_MISSING);
  CHECK(rejects([&] { kit::read_file(file, job); }));
  return failures ? 1 : 0;
}
