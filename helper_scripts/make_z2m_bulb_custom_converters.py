import argparse
from jinja2 import Environment, FileSystemLoader, select_autoescape
from pathlib import Path
import yaml


env = Environment(
    loader=FileSystemLoader("helper_scripts/templates"),
    autoescape=select_autoescape(),
    trim_blocks=True,
    lstrip_blocks=True,
)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Create Zigbee2mqtt converter for custom bulb devices",
        epilog="Generates a js file that adds support of re-flashed bulbs to z2m",
    )
    parser.add_argument(
        "db_file", metavar="INPUT", type=str, help="File with device db"
    )
    parser.add_argument(
        "--z2m-v1", action=argparse.BooleanOptionalAction, help="Use old z2m"
    )

    args = parser.parse_args()

    db_str = Path(args.db_file).read_text()
    db = yaml.safe_load(db_str)

    devices = []

    for device_name, device in db.items():

        # Skip if build == no. Defaults to yes
        if not device.get("build", True):
            continue
        
        # Only include bulb devices (those with ULRT config)
        config = device["config_str"]
        if not config or 'U' not in config.split(';')[2:]:
            continue
      
        zb_manufacturer, zb_model, *peripherals = config.rstrip(";").split(";")

        # Simple bulb device - just one light endpoint
        devices.append({
            "zb_models": [zb_model] + (device.get("old_zb_models") or []),
            "model": device.get("override_z2m_device") or device["stock_converter_model"],
            "isBulb": True,
        })

    template = env.get_template("switch_custom.js.jinja")

    print(template.render(devices=devices, z2m_v1=args.z2m_v1))
