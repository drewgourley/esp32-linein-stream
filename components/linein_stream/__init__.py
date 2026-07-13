import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID, CONF_SAMPLE_RATE, CONF_CHANNELS, CONF_PORT

CODEOWNERS = ["@drewgourley"]
DEPENDENCIES = ["network"]

CONF_BCLK_PIN = "bclk_pin"
CONF_LRCLK_PIN = "lrclk_pin"
CONF_DIN_PIN = "din_pin"
CONF_MCLK_PIN = "mclk_pin"
CONF_MCLK_MULTIPLE = "mclk_multiple"
CONF_I2S_MODE = "i2s_mode"
CONF_I2S_PORT = "i2s_port"
CONF_I2S_FORMAT = "i2s_format"
CONF_GAIN = "gain"
CONF_EQUALIZER = "equalizer"
CONF_TYPE = "type"
CONF_FREQUENCY = "frequency"
CONF_Q = "q"

EQ_BAND_TYPES = {"peaking": 0, "low_shelf": 1, "high_shelf": 2}

linein_stream_ns = cg.esphome_ns.namespace("linein_stream")
LineInStreamComponent = linein_stream_ns.class_("LineInStreamComponent", cg.Component)

EQ_BAND_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_TYPE, default="peaking"): cv.one_of(*EQ_BAND_TYPES, lower=True),
        cv.Required(CONF_FREQUENCY): cv.float_range(min=20, max=20000),
        cv.Optional(CONF_Q, default=0.707): cv.positive_float,
        cv.Optional(CONF_GAIN, default=0.0): cv.float_,
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(LineInStreamComponent),
        cv.Required(CONF_BCLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_LRCLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_DIN_PIN): pins.internal_gpio_input_pin_number,
        cv.Optional(CONF_MCLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_MCLK_MULTIPLE, default=256): cv.one_of(
            128, 256, 384, 512, 768, int=True
        ),
        cv.Optional(CONF_I2S_MODE, default="master"): cv.one_of(
            "master", "slave", lower=True
        ),
        cv.Optional(CONF_I2S_PORT, default=0): cv.int_range(min=0, max=1),
        cv.Optional(CONF_I2S_FORMAT, default="philips"): cv.one_of(
            "philips", "msb", lower=True
        ),
        cv.Optional(CONF_GAIN, default=1.0): cv.positive_float,
        cv.Optional(CONF_EQUALIZER): cv.ensure_list(EQ_BAND_SCHEMA),
        cv.Optional(CONF_SAMPLE_RATE, default=44100): cv.int_range(min=8000, max=48000),
        cv.Optional(CONF_CHANNELS, default=2): cv.int_range(min=1, max=2),
        cv.Optional(CONF_PORT, default=8080): cv.port,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_bclk_pin(config[CONF_BCLK_PIN]))
    cg.add(var.set_lrclk_pin(config[CONF_LRCLK_PIN]))
    cg.add(var.set_din_pin(config[CONF_DIN_PIN]))
    if CONF_MCLK_PIN in config:
        cg.add(var.set_mclk_pin(config[CONF_MCLK_PIN]))
    cg.add(var.set_mclk_multiple(config[CONF_MCLK_MULTIPLE]))
    cg.add(var.set_master(config[CONF_I2S_MODE] == "master"))
    cg.add(var.set_i2s_port(config[CONF_I2S_PORT]))
    cg.add(var.set_use_msb(config[CONF_I2S_FORMAT] == "msb"))
    cg.add(var.set_gain(config[CONF_GAIN]))
    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))
    cg.add(var.set_channels(config[CONF_CHANNELS]))
    cg.add(var.set_port(config[CONF_PORT]))

    for band in config.get(CONF_EQUALIZER, []):
        cg.add(
            var.add_eq_band(
                EQ_BAND_TYPES[band[CONF_TYPE]],
                band[CONF_FREQUENCY],
                band[CONF_Q],
                band[CONF_GAIN],
            )
        )
