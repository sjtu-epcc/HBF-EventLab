"""Read canonical INI exports and check requested settings before analysis."""
from dataclasses import dataclass
from pathlib import Path
from .model import SPEC_PROFILE

OUTPUT_PATHS = {"output.statistics_json", "output.effective_config"}


def parse_configuration(text: str) -> dict[str, str]:
    # The exporter repeats section headers, but never keys. ConfigParser's
    # strict=False would silently accept duplicate keys as well, so do not use it.
    section, result = "", {}
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip()
            if not section:
                raise ValueError(f"empty section at line {number}")
            continue
        key, separator, value = line.partition("=")
        if not section or not separator or not key.strip() or not value.strip():
            raise ValueError(f"invalid configuration at line {number}")
        qualified = section + "." + key.strip()
        if qualified in result:
            raise ValueError(f"duplicate configuration key: {qualified}")
        result[qualified] = value.strip()
    return result


def read_configuration(path: Path) -> dict[str, str]:
    return parse_configuration(path.read_text(encoding="utf-8-sig"))


def normalized_configuration(path: Path, ignored=OUTPUT_PATHS) -> dict[str, str]:
    return {k: v for k, v in read_configuration(path).items() if k not in ignored}


def scalar(value: str):
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] == '"':
        return value[1:-1]
    if value in ("true", "false"):
        return value == "true"
    try:
        return int(value, 16 if value.lower().startswith("0x") else 10)
    except ValueError:
        return value


@dataclass(frozen=True)
class RunConfiguration:
    values: dict[str, str]

    def integer(self, key: str) -> int:
        value = scalar(self.values[key])
        if type(value) is not int or value < 0:
            raise ValueError(f"expected nonnegative integer: {key}")
        return value

    def text(self, key: str) -> str:
        return str(scalar(self.values[key]))

    @property
    def pages_per_block(self):
        return self.integer("device.geometry.pages_per_block")

    @property
    def channels(self):
        return self.integer("device.geometry.host_channels")

    @property
    def selected_mocs(self):
        return self.integer("device.selected.mocs_selected")

    @property
    def pending_limit(self):
        return self.integer("device.capabilities.max_pending_write_dlus")


def load_run_configuration(directory: Path) -> RunConfiguration:
    actual = RunConfiguration(read_configuration(directory / "effective.ini"))
    expected = read_configuration(directory / "input.ini")
    if actual.text("device.spec_profile") != SPEC_PROFILE:
        raise ValueError("unsupported experiment model profile")
    for key, requested in expected.items():
        if key in OUTPUT_PATHS:
            continue  # CLI sets these output paths separately.
        wanted = scalar(requested)
        if key == "device.selected.zone_size_dlus" and wanted == "auto":
            wanted = (actual.pages_per_block * actual.integer("device.geometry.dies_per_channel")
                      * actual.integer("device.geometry.banks_per_die"))
        if key not in actual.values or scalar(actual.values[key]) != wanted:
            raise ValueError(f"effective configuration differs from request: {key}")
    if min(actual.channels, actual.pages_per_block, actual.selected_mocs, actual.pending_limit) <= 0:
        raise ValueError("invalid effective geometry/resource budget")
    if actual.selected_mocs > actual.integer("device.capabilities.mocs_supported"):
        raise ValueError("Selected MOCS exceeds Supported")
    return actual
