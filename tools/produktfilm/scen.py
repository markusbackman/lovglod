"""
Bygger produktfilmens scen i Blender och renderar den. Körs av bygg.py:

    blender -b --factory-startup -P tools/produktfilm/scen.py -- [flaggor]

    --kvalitet utkast|film|final   upplösning och sampel (utkast är standard)
    --rutor 1-1140            bara de här rutorna
    --stillbild 12,140,300    bara enstaka rutor
    --ut <katalog>            vart rutorna skrivs
    --spara <fil.blend>       spara scenen också, för att öppna i Blender

Lampan sätts ihop av STL-filerna i hardware/v2 precis som MONTERA.md
beskriver: lövet 30,5 mm upp i sockeln och 32 mm in från sockelns front.

Ljuset i det gula fönstret är inte en animerad färg. Fönstret vet för varje
punkt var längs listen den ligger — uppmätt mot kärnans yttervägg, där listen
sitter — och slår upp diodens färg i ljusspåret från ljusspar.py. Samma list,
samma dioder, samma lägen som lampan.
"""

import math
import sys
from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

HAR = Path(__file__).resolve().parent
ROT = HAR.parents[1]
sys.path.insert(0, str(HAR))
import ljusspar  # noqa: E402
import manus  # noqa: E402

HW = ROT / "hardware" / "v2"

# Lövets läge i sockeln, i mm. Totalhöjden 250,5 mm minus lövets 220 mm ger
# 30,5; sockelns krage är 29,4 mm djup och centrerad kring y = 42.
LOV_UPP = 30.5
LOV_IN = 32.0

KVALITET = {
    "utkast": dict(res=(960, 540), sampel=24, brus=0.05),
    "film": dict(res=(1920, 1080), sampel=32, brus=0.04),
    "final": dict(res=(1920, 1080), sampel=192, brus=0.012),
}

# Färgerna, sRGB. Grönt och vitt är filamenten, gult det genomskinliga fönstret.
GRON = (0.075, 0.30, 0.20)
VIT = (0.93, 0.93, 0.90)
GUL = (1.0, 0.80, 0.10)

# Hur starkt en diod på full styrka lyser i fönstret, i Blenders
# emissionsenheter. Justerat mot hur lampan ser ut på film — gnistorna ska
# bränna ut mot vitt, standbyglöden ska synas men inte blända.
LED_STYRKA = 11.0
# Dioderna genom fönstret: avståndet mellan dem längs listen (väggen 488 mm
# delad på antalet dioder), hur stor ljuspunkten kring varje diod är, och hur
# starkt det spridda skenet mellan dem lyser jämfört med punkten.
LED_DELNING_MM = 488.2 / ljusspar.N
LED_PUNKT_MM = 3.2
LED_SKEN = 0.18
LED_PUNKT_STYRKA = 1.8


def args():
    a = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    o = dict(kvalitet="utkast", rutor=None, stillbild=None, ut=str(HAR / "ut" / "rutor"), spara=None)
    for i in range(0, len(a), 2):
        o[a[i].lstrip("-")] = a[i + 1]
    return o


def srgb(c):
    return tuple(x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c) + (1.0,)


# ── Geometri ─────────────────────────────────────────────────────────────────
def mm(m):
    """Filens mm → världens meter, med en 4×4 i mm."""
    return Matrix.Scale(0.001, 4) @ Matrix(m)


# Filerna är ritade med framsidan nedåt (z = 0 är framsidan) och y uppåt.
# Världen: x åt höger sett framifrån, y bakåt, z upp. (x, y, z) → (−x, z, y)
# är en ren rotation, inte en spegling, så texten läses rätt.
LOV = mm([[-1, 0, 0, 0], [0, 0, 1, LOV_IN], [0, 1, 0, LOV_UPP], [0, 0, 0, 1]])
SOCKEL = mm([[-1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 0], [0, 0, 0, 1]])
# Bakstycket och dess gula kontur exporteras med utsidan nedåt: ett halvt
# varv kring y, så filens z = 0 är ytan som vetter bakåt ut ur lampan. Här vrids
# de tillbaka — x och z byter tecken — och läggs på kärnan: insidan på z = 15,
# utsidan på 17, 3 mm under lövets baksida.
BAK = LOV @ Matrix([[-1, 0, 0, 0], [0, 1, 0, 0], [0, 0, -1, 17], [0, 0, 0, 1]])
# Bottenplattan är utskriven upp och ner: det som var uppåt är lampans botten.
PLATTA = mm([[-1, 0, 0, 0], [0, 1, 0, 42], [0, 0, -1, 0], [0, 0, 0, 1]])


def lampdelar():
    c = bpy.data.collections.get("lampan")
    if not c:
        c = bpy.data.collections.new("lampan")
        bpy.context.scene.collection.children.link(c)
    return c


def importera(fil, matris, material, namn):
    bpy.ops.wm.stl_import(filepath=str(fil))
    obj = bpy.context.selected_objects[0]
    obj.name = namn
    # Studioljuset lyser bara på lampan, se studio().
    for c in obj.users_collection:
        c.objects.unlink(obj)
    lampdelar().objects.link(obj)
    me = obj.data
    if namn == "fonster":
        # Filens xy följer med som attribut. Det är linjärt i positionen, så det
        # interpoleras exakt över trianglarna hur grovt fönstret än är
        # trianglerat.
        attr = me.attributes.new("fil", "FLOAT2", "POINT")
        attr.data.foreach_set("vector", [c for v in me.vertices for c in (v.co.x, v.co.y)])
    me.transform(matris)
    if matris.determinant() < 0:
        me.flip_normals()
    me.materials.append(material)
    # Utskrivna delar har skarpa kanter; auto-smooth håller de platta ytorna
    # platta och de rundade lenta.
    bpy.ops.object.shade_auto_smooth(angle=math.radians(35))
    return obj


def karnans_kontur():
    """Kärnans yttervägg i filens xy — det listen sitter mot. Ordnad från
    lövets nedersta punkt, där listen börjar."""
    bpy.ops.wm.stl_import(filepath=str(HW / "plate-1" / "bjorkloven_core.stl"))
    obj = bpy.context.selected_objects[0]
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    r = bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:],
                               plane_co=(0, 0, 8), plane_no=(0, 0, 1))
    kanter = [e for e in r["geom_cut"] if isinstance(e, bmesh.types.BMEdge)]
    grann = {}
    for e in kanter:
        a, b = e.verts
        grann.setdefault(a, []).append(b)
        grann.setdefault(b, []).append(a)
    # Sammanhängande slingor; ytterväggen är den med störst utbredning.
    sedda, slingor = set(), []
    for v in grann:
        if v in sedda:
            continue
        slinga, prev, cur = [], None, v
        while cur not in sedda:
            sedda.add(cur)
            slinga.append(cur.co.xy.copy())
            nxt = [n for n in grann[cur] if n is not prev and n not in sedda]
            if not nxt:
                break
            prev, cur = cur, nxt[0]
        slingor.append(slinga)
    bm.free()
    bpy.data.objects.remove(obj)

    def utbredning(s):
        xs, ys = [p.x for p in s], [p.y for p in s]
        return (max(xs) - min(xs)) * (max(ys) - min(ys))

    s = max(slingor, key=utbredning)
    start = min(range(len(s)), key=lambda i: s[i].y)
    s = s[start:] + s[:start]
    pts = np.array([(p.x, p.y) for p in s + [s[0]]])
    langd = np.linalg.norm(np.diff(pts, axis=0), axis=1).sum()
    print(f"[scen] listens vägg: {langd:.1f} mm")
    return pts


def uppslag(kontur, x0=-93.0, y0=18.0, w=186.0, h=200.0, px_mm=5):
    """Bild över fönstret: R = läge längs listen (0–1), G = avstånd till
    listen i mm. Fönstrets shader slår upp sin punkt här."""
    nx, ny = int(w * px_mm), int(h * px_mm)
    xs = x0 + (np.arange(nx) + 0.5) / px_mm
    ys = y0 + (np.arange(ny) + 0.5) / px_mm
    P = np.stack(np.meshgrid(xs, ys), -1).reshape(-1, 2)
    A, B = kontur[:-1], kontur[1:]
    AB = B - A
    seg = np.linalg.norm(AB, axis=1)
    s0 = np.concatenate([[0], np.cumsum(seg)[:-1]])
    L = seg.sum()
    u = np.empty(len(P))
    d = np.empty(len(P))
    for i in range(0, len(P), 4096):
        p = P[i:i + 4096, None, :]
        t = np.clip(((p - A) * AB).sum(-1) / (seg ** 2), 0, 1)
        q = A + t[..., None] * AB
        dist = np.linalg.norm(p - q, axis=-1)
        k = dist.argmin(1)
        r = np.arange(len(k))
        u[i:i + 4096] = (s0[k] + t[r, k] * seg[k]) / L
        d[i:i + 4096] = dist[r, k]
    rgba = np.stack([u, d, np.zeros_like(u), np.ones_like(u)], -1).astype(np.float32)
    img = bpy.data.images.new("uppslag", nx, ny, alpha=True, float_buffer=True)
    img.colorspace_settings.name = "Non-Color"
    img.pixels.foreach_set(rgba.ravel())
    img.pack()
    return img, (x0, y0, w, h)


def ljusbild():
    """Ljusspåret som bild: en kolumn per diod, en rad per filmruta, linjärt
    ljus 0–1 med lampans ljusstyrka inräknad."""
    rutor = ljusspar.rakna_ut()
    k = ljusspar.BRIGHTNESS / 255 / 255
    px = np.array([[[c[0] * k, c[1] * k, c[2] * k, 1.0] for c in r] for r in rutor], np.float32)
    img = bpy.data.images.new("ljusspar", ljusspar.N, len(rutor), alpha=True, float_buffer=True)
    img.colorspace_settings.name = "Non-Color"
    img.pixels.foreach_set(px.ravel())
    img.pack()
    return img, len(rutor)


# ── Material ─────────────────────────────────────────────────────────────────
def principled(namn, farg, grovhet, **extra):
    m = bpy.data.materials.new(namn)
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = srgb(farg)
    b.inputs["Roughness"].default_value = grovhet
    for k, v in extra.items():
        b.inputs[k].default_value = v
    return m, b


def fonster_material(uppslag_img, ruta, ljus_img, n_rutor):
    m, b = principled("fonster", GUL, 0.32,
                      **{"Transmission Weight": 0.55, "Subsurface Weight": 0.25,
                         "Subsurface Radius": (1.0, 0.6, 0.1), "Subsurface Scale": 0.002})
    nt, N = m.node_tree, m.node_tree.nodes
    L = nt.links.new

    attr = N.new("ShaderNodeAttribute")
    attr.attribute_type = "GEOMETRY"
    attr.attribute_name = "fil"
    x0, y0, w, h = ruta
    kart = N.new("ShaderNodeMapping")
    kart.vector_type = "POINT"
    kart.inputs["Location"].default_value = (-x0 / w, -y0 / h, 0)
    kart.inputs["Scale"].default_value = (1 / w, 1 / h, 1)
    L(attr.outputs["Vector"], kart.inputs["Vector"])

    upp = N.new("ShaderNodeTexImage")
    upp.image = uppslag_img
    upp.interpolation = "Closest"
    upp.extension = "EXTEND"
    L(kart.outputs["Vector"], upp.inputs["Vector"])
    sep = N.new("ShaderNodeSeparateColor")
    L(upp.outputs["Color"], sep.inputs["Color"])

    # Vilken filmruta: nyckelrutor på ett värde, linjärt från ruta 1 till sista.
    rad = N.new("ShaderNodeValue")
    rad.name = "rad"
    ut = rad.outputs[0]
    ut.default_value = 0.5 / n_rutor
    ut.keyframe_insert("default_value", frame=1)
    ut.default_value = (n_rutor - 0.5) / n_rutor
    ut.keyframe_insert("default_value", frame=n_rutor)
    for fc in m.node_tree.animation_data.action.layers[0].strips[0].channelbags[0].fcurves:
        for kp in fc.keyframe_points:
            kp.interpolation = "LINEAR"

    def matte(op, x, y=None, namn=None):
        n = N.new("ShaderNodeMath")
        n.operation = op
        for i, v in enumerate((x, y)):
            if v is None:
                continue
            if isinstance(v, (int, float)):
                n.inputs[i].default_value = v
            else:
                L(v, n.inputs[i])
        return n.outputs[0]

    def las(u, interp):
        xyz = N.new("ShaderNodeCombineXYZ")
        L(u, xyz.inputs["X"])
        L(rad.outputs[0], xyz.inputs["Y"])
        t = N.new("ShaderNodeTexImage")
        t.image = ljus_img
        t.interpolation = interp
        t.extension = "EXTEND"
        L(xyz.outputs["Vector"], t.inputs["Vector"])
        return t.outputs["Color"]

    # Genom fönstret syns varje diod som en egen ljuspunkt med guldglöd
    # emellan — så ser den riktiga lampan ut på film, och det är punkterna som
    # gör att jagande block, kometer och gnistor går att läsa. Två lager:
    #
    #   punkt  närmaste diodens färg, skarpt, i en gaussisk fläck kring dioden
    #   sken   linjärt blandat mellan grannarna, svagt — ljuset som sprids i
    #          fönstret
    #
    # Diodernas mittpunkter ligger på (i + 0,5)/N längs listen, och så ligger
    # också ljusbildens texelmitt.
    u = sep.outputs["Red"]
    x = matte("MULTIPLY", u, ljusspar.N)
    narmast = matte("DIVIDE", matte("ADD", matte("FLOOR", x), 0.5), ljusspar.N)
    ifran = matte("MULTIPLY", matte("ABSOLUTE", matte("SUBTRACT", matte("FRACT", x), 0.5)),
                  LED_DELNING_MM)
    punkt = matte("MULTIPLY", LED_PUNKT_STYRKA,
                  matte("EXPONENT", matte("MULTIPLY", matte("POWER", matte("DIVIDE", ifran, LED_PUNKT_MM), 2), -1)))

    mix = N.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    mix.blend_type = "ADD"
    mix.clamp_result = False
    mix.inputs["Factor"].default_value = 1.0
    sken = N.new("ShaderNodeMix")
    sken.data_type = "RGBA"
    sken.blend_type = "MULTIPLY"
    sken.inputs["Factor"].default_value = 1.0
    L(las(u, "Linear"), sken.inputs["A"])
    sken.inputs["B"].default_value = (LED_SKEN,) * 3 + (1,)
    karna = N.new("ShaderNodeMix")
    karna.data_type = "RGBA"
    karna.blend_type = "MULTIPLY"
    karna.inputs["Factor"].default_value = 1.0
    karna.clamp_result = False
    L(las(narmast, "Closest"), karna.inputs["A"])
    fp = N.new("ShaderNodeCombineColor")
    for k in ("Red", "Green", "Blue"):
        L(punkt, fp.inputs[k])
    L(fp.outputs["Color"], karna.inputs["B"])
    L(sken.outputs["Result"], mix.inputs["A"])
    L(karna.outputs["Result"], mix.inputs["B"])

    # Ljuset är starkast närmast listen och klingar av in över bandet.
    fall = N.new("ShaderNodeMapRange")
    fall.inputs["To Min"].default_value = 0.35
    fall.inputs["To Max"].default_value = 1.0
    L(matte("EXPONENT", matte("MULTIPLY", sep.outputs["Green"], -1 / 7.0)), fall.inputs["Value"])
    styrka = matte("MULTIPLY", fall.outputs["Result"], LED_STYRKA)

    L(mix.outputs["Result"], b.inputs["Emission Color"])
    L(styrka, b.inputs["Emission Strength"])
    return m


# ── Studion ──────────────────────────────────────────────────────────────────
def studio(n_rutor):
    # Svart, blank golvyta som speglar lampan mjukt — och ljuset från den.
    bpy.ops.mesh.primitive_plane_add(size=12, location=(0, 0, -0.0046))
    golv = bpy.context.object
    golv.name = "golv"
    m, b = principled("golv", (0.012, 0.013, 0.013), 0.22,
                      **{"Specular IOR Level": 0.6})
    golv.data.materials.append(m)

    w = bpy.context.scene.world = bpy.data.worlds.new("svart")
    w.use_nodes = True
    w.node_tree.nodes["Background"].inputs["Color"].default_value = (0.0, 0.0, 0.0, 1)

    mitt = Vector((0, 0.042, 0.13))

    def ljus(namn, pos, storlek, energi, farg=(1, 1, 1), form="RECTANGLE", sy=None):
        d = bpy.data.lights.new(namn, "AREA")
        d.shape = form
        d.size = storlek
        d.size_y = sy or storlek
        d.energy = energi
        d.color = farg
        o = bpy.data.objects.new(namn, d)
        bpy.context.collection.objects.link(o)
        o.location = pos
        o.rotation_euler = (mitt - Vector(pos)).to_track_quat("-Z", "Y").to_euler()
        # Ljuskällorna syns aldrig själva, bara det de lyser på.
        o.visible_camera = False
        # ...och bara lampan. Golvet speglade annars softboxarna som vita
        # fält; nu speglar det bara lampans eget ljus.
        o.light_linking.receiver_collection = lampdelar()
        return d

    fyll = [
        ljus("nyckel", (-0.9, -0.85, 0.95), 1.1, 90, (1.0, 0.97, 0.93), sy=0.7),
        ljus("tak", (0.1, -0.1, 1.3), 0.9, 18),
    ]
    kant = [
        ljus("kant_v", (-0.55, 0.55, 0.32), 0.08, 45, (0.85, 0.92, 1.0), sy=0.9),
        ljus("kant_h", (0.6, 0.5, 0.36), 0.08, 45, (0.85, 0.92, 1.0), sy=0.9),
    ]
    # Ljuskurvorna ligger i manuset: FYLL visar lampan, KANT ritar konturen.
    for lampor, kurva in ((fyll, manus.FYLL), (kant, manus.KANT)):
        for d in lampor:
            full = d.energy
            for t, f in kurva:
                d.energy = full * f
                d.keyframe_insert("energy", frame=t * manus.FPS + 1)
            for kp in fcurves(d)[0].keyframe_points:
                kp.interpolation = "LINEAR"


def fcurves(id_):
    return id_.animation_data.action.layers[0].strips[0].channelbags[0].fcurves


def kameror():
    sc = bpy.context.scene
    for t in manus.TAGNINGAR:
        f0 = round(t["start"] * manus.FPS) + 1
        f1 = round(t["slut"] * manus.FPS) + 1
        mal = bpy.data.objects.new("mal_" + t["namn"], None)
        sc.collection.objects.link(mal)
        cd = bpy.data.cameras.new(t["namn"])
        cd.lens = t["lins"]
        cd.sensor_width = 36
        cd.clip_start = 0.01
        cd.dof.use_dof = True
        cd.dof.focus_object = mal
        cd.dof.aperture_fstop = t["bland"]
        cd.dof.aperture_blades = 9
        cd.shift_x = t.get("skift", 0.0)
        cam = bpy.data.objects.new(t["namn"], cd)
        sc.collection.objects.link(cam)
        c = cam.constraints.new("TRACK_TO")
        c.target = mal
        c.track_axis = "TRACK_NEGATIVE_Z"
        c.up_axis = "UP_Y"
        for f, (pos, sikte) in ((f0, t["fran"]), (f1, t["till"])):
            cam.location = Vector(pos) * 0.001
            mal.location = Vector(sikte) * 0.001
            cam.keyframe_insert("location", frame=f)
            mal.keyframe_insert("location", frame=f)
        for o in (cam, mal):
            for fc in fcurves(o):
                # Full fart in i klippet, inbromsning mot slutet.
                forsta = fc.keyframe_points[0]
                forsta.interpolation = "SINE"
                forsta.easing = "EASE_OUT"
        sc.timeline_markers.new(t["namn"], frame=f0).camera = cam
        if f0 == 1:
            sc.camera = cam


def kompositor():
    sc = bpy.context.scene
    ng = bpy.data.node_groups.new("komp", "CompositorNodeTree")
    ng.interface.new_socket("Image", in_out="OUTPUT", socket_type="NodeSocketColor")
    sc.compositing_node_group = ng
    rl = ng.nodes.new("CompositorNodeRLayers")
    glare = ng.nodes.new("CompositorNodeGlare")
    glare.inputs["Type"].default_value = "Bloom"
    glare.inputs["Quality"].default_value = "High"
    glare.inputs["Threshold"].default_value = 0.9
    glare.inputs["Strength"].default_value = 0.35
    glare.inputs["Size"].default_value = 0.6
    ut = ng.nodes.new("NodeGroupOutput")
    ng.links.new(rl.outputs["Image"], glare.inputs["Image"])
    ng.links.new(glare.outputs["Image"], ut.inputs[0])


def rendering(q, n_rutor, ut):
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    pref = bpy.context.preferences.addons["cycles"].preferences
    pref.compute_device_type = "METAL"
    pref.get_devices()
    # Bara GPU:n. Med CPU:n påslagen också delar Cycles upp bilden mellan dem,
    # och på Apple Silicon blir det långsammare än GPU:n ensam.
    for d in pref.devices:
        d.use = d.type == "METAL"
    sc.cycles.device = "GPU"
    sc.cycles.samples = q["sampel"]
    sc.cycles.adaptive_threshold = q["brus"]
    sc.cycles.use_denoising = True
    sc.cycles.denoiser = "OPENIMAGEDENOISE"
    sc.cycles.max_bounces = 8
    sc.cycles.transmission_bounces = 8
    sc.render.use_persistent_data = True
    sc.render.use_motion_blur = True
    sc.render.motion_blur_shutter = 0.5
    sc.render.resolution_x, sc.render.resolution_y = q["res"]
    sc.render.resolution_percentage = 100
    sc.render.fps = manus.FPS
    sc.frame_start, sc.frame_end = 1, n_rutor
    sc.view_settings.view_transform = "AgX"
    sc.view_settings.look = "AgX - Medium High Contrast"
    sc.render.image_settings.file_format = "PNG"
    sc.render.image_settings.color_depth = "16"
    sc.render.image_settings.compression = 15
    sc.render.compositor_device = "GPU"
    sc.render.filepath = str(Path(ut) / "####")
    # Rutor som redan finns renderas inte om, och en ruta som påbörjats får en
    # platshållare — så går det att avbryta och fortsätta, eller köra flera
    # Blender samtidigt på samma katalog.
    sc.render.use_overwrite = False
    sc.render.use_placeholder = True


def bygg():
    o = args()
    bpy.ops.wm.read_factory_settings(use_empty=True)

    kontur = karnans_kontur()
    upp_img, ruta = uppslag(kontur)
    ljus_img, n = ljusbild()

    gron, _ = principled("gron", GRON, 0.48, **{"Specular IOR Level": 0.45,
                                                "Subsurface Weight": 0.08})
    vit, _ = principled("vit", VIT, 0.42, **{"Subsurface Weight": 0.15,
                                             "Subsurface Scale": 0.001})
    fonster = fonster_material(upp_img, ruta, ljus_img, n)
    # Bakstyckets kontur är gul PETG. Den syns inte framifrån, men den finns
    # med så att lampan är hel från alla håll.
    petg, _ = principled("kontur", GUL, 0.35, **{"Transmission Weight": 0.5})

    p1, p3 = HW / "plate-1", HW / "plate-3"
    importera(p1 / "bjorkloven_sign.stl", LOV, gron, "skal")
    importera(p1 / "bjorkloven_core.stl", LOV, gron, "karna")
    importera(p1 / "bjorkloven_window.stl", LOV, fonster, "fonster")
    importera(p1 / "bjorkloven_letters.stl", LOV, vit, "bokstaver")
    importera(HW / "plate-2" / "bjorkloven_back.stl", BAK, gron, "bak")
    importera(HW / "plate-2" / "bjorkloven_back_glow.stl", BAK, petg, "kontur")
    importera(p3 / "bjorkloven_base.stl", SOCKEL, gron, "sockel")
    importera(p3 / "bjorkloven_base_letters.stl", SOCKEL, vit, "sockeltext")
    importera(p3 / "bjorkloven_plate.stl", PLATTA, gron, "platta")

    studio(n)
    kameror()
    kompositor()
    q = KVALITET[o["kvalitet"]]
    rendering(q, n, o["ut"])

    if o["spara"]:
        bpy.ops.wm.save_as_mainfile(filepath=str(Path(o["spara"]).resolve()))

    sc = bpy.context.scene
    if o["stillbild"]:
        for f in (int(x) for x in o["stillbild"].split(",")):
            sc.frame_set(f)
            sc.render.filepath = str(Path(o["ut"]) / f"{f:04d}")
            bpy.ops.render.render(write_still=True)
    else:
        if o["rutor"]:
            a, b = (int(x) for x in o["rutor"].split("-"))
            sc.frame_start, sc.frame_end = a, b
        bpy.ops.render.render(animation=True)


bygg()
