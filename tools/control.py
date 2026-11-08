#!/usr/bin/env python3
"""Configure the TF2VR neural add-on; a running game reloads on its next poll."""
import argparse
import configparser
from pathlib import Path
import shlex

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('action',choices=['status','on','off','quality','strength','tone','detail','model','dlaa','fg'])
parser.add_argument('value',nargs='?')
parser.add_argument('--test',action='store_true',help='Use the isolated test profile')
args=parser.parse_args()
if args.value is not None and args.action not in ('quality','strength','tone','detail','model','dlaa','fg'):
    parser.error('This action does not accept a value')
if args.action in ('quality','strength','tone','detail'):
    try: args.value=int(args.value)
    except (ValueError,TypeError): parser.error(f'{args.action} requires a whole number')
settings={}
profile='TF2VR-Autotest' if args.test else 'TF2VR'
layer=Path(__file__).resolve().parent/'plugins/layer'
if args.test or not (layer/'TF2VRNeuralRendering.ini').is_file():
    paths=Path.home()/'.config/tf2vr/paths.env'
    if not paths.is_file():parser.error('Run the control.py inside the installed addon package, or configure ~/.config/tf2vr/paths.env')
    for line in paths.read_text().splitlines():
        if line.strip() and not line.startswith('#'):
            key,value=line.split('=',1);settings[key]=shlex.split(value)[0] if value else ''
    layer=Path(settings['GAME_DIR'])/profile/'packages/Local-TF2VRNeuralRendering-0.1.0/plugins/layer'
path=layer/'TF2VRNeuralRendering.ini'
config=configparser.ConfigParser()
if not path.is_file():parser.error(f'Add-on configuration missing: {path}')
config.read(path)
if not config.has_section('neural_rendering'):config.add_section('neural_rendering')
section=config['neural_rendering']
if args.action in ('on','off'):
    if args.action=='on' and not (layer/'runtime/nvngx_dlssnr.dll').is_file():parser.error('The NVIDIA NR runtime is missing from this add-on')
    section['enabled']='1' if args.action=='on' else '0'
elif args.action=='quality':
    if args.value not in (25,50,75,100):parser.error('quality must be 25, 50, 75 or 100 (percent of each eye dimension)')
    section['resolution_percent']=str(args.value)
elif args.action in ('strength','tone','detail'):
    if not 0<=args.value<=100:parser.error(f'{args.action} must be 0 to 100')
    section[{'strength':'intensity_percent','tone':'tone_percent','detail':'detail_percent'}[args.action]]=str(args.value)
elif args.action=='model':
    if args.value is None or args.value.upper() not in tuple('AB'):parser.error('model must be A or B')
    section['model']=args.value.upper()
elif args.action=='dlaa':
    if args.value not in ('on','off'):parser.error('dlaa requires on or off')
    if args.value=='on' and not (layer/'runtime/nvngx_dlss.dll').is_file():parser.error('The NVIDIA DLSS/DLAA runtime is missing from this add-on')
    if not config.has_section('dlaa'):config.add_section('dlaa')
    config['dlaa']['enabled']='1' if args.value=='on' else '0'
elif args.action=='fg':
    if args.value not in ('on','off'):parser.error('fg requires on or off')
    if args.value=='on' and not (layer/'runtime/nvngx_dlssg.dll').is_file():parser.error('The NVIDIA frame-generation runtime is missing from this add-on')
    if not config.has_section('frame_generation'):config.add_section('frame_generation')
    config['frame_generation']['enabled']='1' if args.value=='on' else '0'
if args.action!='status':
    tmp=path.with_suffix('.tmp')
    with tmp.open('w') as f:config.write(f)
    tmp.replace(path)
    (layer/'neural.request').touch()
print(f'{profile}: neural rendering configured {"ON" if section.getboolean("enabled",False) else "OFF"}; model resolution {section.get("resolution_percent","50")}% per dimension; strength {section.get("intensity_percent","50")}%')
print(f'Tone {section.get("tone_percent","100")}% ; detail {section.get("detail_percent","100")}% ; model {section.get("model","B")} ; layer {"ON" if config.getboolean("layer","enabled",fallback=True) else "OFF (restart required after enabling)"}')
print(f'DLAA configured {"ON" if config.getboolean("dlaa","enabled",fallback=False) else "OFF"} (experimental; independent of neural rendering)')
print(f'2x VR frame generation configured {"ON" if config.getboolean("frame_generation","enabled",fallback=False) else "OFF"} (experimental; requires valid stereo guides)')
if args.action=='status':
    log=layer/'TF2VRNeuralRendering.log'
    if log.exists():
        print('Recent processing log (may be from an earlier game session):')
        with log.open('rb') as f:
            f.seek(max(0,log.stat().st_size-16000));lines=f.read().decode(errors='replace').splitlines()
        for line in [x for x in lines if any(k in x for k in ('neural_evaluate','dlaa_evaluate','dlaa_disabled','neural_init_failed','neural_render_failed','neural_backend_shutdown','fg_present','fg_pending','fg_evaluate_failed','fg_init_failed','fg_settings_reloaded'))][-8:]:print(line)
else:print('The running game will reload this setting. Starting the game also reads it.')
