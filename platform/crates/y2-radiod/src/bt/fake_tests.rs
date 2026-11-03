//! The controller against a fake BlueZ and BlueALSA on a private bus (libdbus takes the "system" bus
//! from DBUS_SYSTEM_BUS_ADDRESS): Bluetooth comes on, the preferred headset is paged after the
//! adoption window, its stream becomes Ready with a generation, and a sleep takes the adapter down.
use super::*;
use crate::arbiter::Event;
use dbus::{
    arg::{PropMap, RefArg, Variant},
    blocking::Connection,
    channel::{Channel, MatchingReceiver, Sender},
    message::MatchRule,
    Message, Path as DbusPath,
};
use std::{
    collections::HashMap,
    io::{BufRead, BufReader},
    process::{Child, Command, Stdio},
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc, Arc, Mutex,
    },
    time::{Duration, Instant},
};
use y2p_contract::radiod::{Link, Radio, Request};

const ADAPTER: &str = "/org/bluez/hci0";
const DEVICE: &str = "/org/bluez/hci0/dev_12_34_56_78_90_AB";
const PCM: &str = "/org/bluealsa/hci0/dev_12_34_56_78_90_AB/a2dpsrc/sink";

#[derive(Default)]
struct Fake {
    powered: bool,
    connected: bool,
    connects: u32,
    in_progress_first: bool,
}

fn variant<T: RefArg + 'static>(value: T) -> Variant<Box<dyn RefArg>> {
    Variant(Box::new(value))
}

type Objects = HashMap<DbusPath<'static>, HashMap<String, PropMap>>;

fn bluez_objects(fake: &Fake) -> Objects {
    let mut adapter = PropMap::new();
    adapter.insert("Powered".into(), variant(fake.powered));
    adapter.insert("Discovering".into(), variant(false));
    let mut device = PropMap::new();
    device.insert("Address".into(), variant("12:34:56:78:90:AB".to_string()));
    device.insert("Alias".into(), variant("Buds".to_string()));
    for key in ["Paired", "Bonded", "Trusted"] {
        device.insert(key.into(), variant(true));
    }
    device.insert("Blocked".into(), variant(false));
    device.insert("Connected".into(), variant(fake.connected));
    device.insert("ServicesResolved".into(), variant(fake.connected));
    device.insert(
        "UUIDs".into(),
        variant(vec![super::world::SINK.to_string()]),
    );
    let mut objects = Objects::new();
    objects.insert(
        DbusPath::from(ADAPTER),
        HashMap::from([("org.bluez.Adapter1".to_string(), adapter)]),
    );
    objects.insert(
        DbusPath::from(DEVICE),
        HashMap::from([("org.bluez.Device1".to_string(), device)]),
    );
    objects
}

fn bluealsa_objects(fake: &Fake) -> Objects {
    let mut objects = Objects::new();
    if fake.connected {
        let mut pcm = PropMap::new();
        pcm.insert("Device".into(), variant(DbusPath::from(DEVICE)));
        pcm.insert("Transport".into(), variant("A2DP-source".to_string()));
        pcm.insert("Mode".into(), variant("sink".to_string()));
        pcm.insert("Codec".into(), variant("SBC".to_string()));
        pcm.insert("Format".into(), variant(0x8210u16));
        pcm.insert("Rate".into(), variant(44_100u32));
        pcm.insert("Channels".into(), variant(2u8));
        pcm.insert("Running".into(), variant(false));
        pcm.insert("Sequence".into(), variant(u32::from(fake.connects as u8)));
        objects.insert(
            DbusPath::from(PCM),
            HashMap::from([("org.bluealsa.PCM1".to_string(), pcm)]),
        );
    }
    objects
}

fn changed(path: &str, interface: &str) -> Message {
    Message::new_signal(path, "org.freedesktop.DBus.Properties", "PropertiesChanged")
        .expect("signal")
        .append3(
            interface,
            PropMap::new(),
            vec!["Powered".to_string(), "Connected".into()],
        )
}

/// Answer one method call as BlueZ or BlueALSA would.
fn answer(fake: &Mutex<Fake>, message: &Message, connection: &Connection) -> Message {
    let member = message.member().map(|m| m.to_string()).unwrap_or_default();
    let path = message.path().map(|p| p.to_string()).unwrap_or_default();
    let mut fake = fake.lock().expect("fake");
    match member.as_str() {
        "GetManagedObjects" if path == "/" => message.return_with_args((bluez_objects(&fake),)),
        "GetManagedObjects" => message.return_with_args((bluealsa_objects(&fake),)),
        "Set" => {
            let (_, name, value): (String, String, Variant<bool>) =
                message.read3().expect("Set arguments");
            if name == "Powered" {
                fake.powered = value.0;
                if !value.0 {
                    fake.connected = false;
                }
                let _ = connection.send(changed(ADAPTER, "org.bluez.Adapter1"));
            }
            message.method_return()
        }
        "ConnectProfile" => {
            fake.connects += 1;
            if fake.in_progress_first && fake.connects == 1 {
                return message.error(&"org.bluez.Error.InProgress".into(), c"In Progress");
            }
            fake.connected = true;
            let _ = connection.send(changed(DEVICE, "org.bluez.Device1"));
            message.method_return()
        }
        "Disconnect" => {
            fake.connected = false;
            let _ = connection.send(changed(DEVICE, "org.bluez.Device1"));
            message.method_return()
        }
        "Get" => message.return_with_args((variant(vec!["A2DP-source:SBC".to_string()]),)),
        "GetCodecs" => {
            let mut sbc = PropMap::new();
            sbc.insert("Channels".into(), variant(vec![1u8, 2u8]));
            sbc.insert("Rates".into(), variant(vec![44_100u32, 48_000u32]));
            message.return_with_args((HashMap::from([("SBC".to_string(), sbc)]),))
        }
        _ => message.method_return(),
    }
}

struct Bus {
    daemon: Child,
    address: String,
}

impl Drop for Bus {
    fn drop(&mut self) {
        let _ = self.daemon.kill();
        let _ = self.daemon.wait();
    }
}

fn private_bus() -> Option<Bus> {
    let mut daemon = Command::new("dbus-daemon")
        .args(["--session", "--nofork", "--nopidfile", "--print-address=1"])
        .stdout(Stdio::piped())
        .spawn()
        .ok()?;
    let mut address = String::new();
    BufReader::new(daemon.stdout.take()?)
        .read_line(&mut address)
        .ok()?;
    Some(Bus {
        daemon,
        address: address.trim().to_owned(),
    })
}

/// Serve BlueZ and BlueALSA on the bus until `stop`.
fn serve(address: String, fake: Arc<Mutex<Fake>>, stop: Arc<AtomicBool>, ready: mpsc::Sender<()>) {
    let mut channel = Channel::open_private(&address).expect("bus");
    channel.register().expect("register");
    let connection = Connection::from(channel);
    for name in ["org.bluez", "org.bluealsa"] {
        connection
            .request_name(name, false, true, false)
            .expect("name");
    }
    connection.start_receive(
        MatchRule::new_method_call(),
        Box::new(move |message, connection| {
            let reply = answer(&fake, &message, connection);
            let _ = connection.send(reply);
            true
        }),
    );
    let _ = ready.send(());
    while !stop.load(Ordering::Acquire) {
        let _ = connection.process(Duration::from_millis(50));
    }
}

/// The newest published state that satisfies `wanted`, within `within`.
fn until(
    events: &mpsc::Receiver<Event>,
    within: Duration,
    wanted: impl Fn(&Bluetooth) -> bool,
) -> Option<Bluetooth> {
    let end = Instant::now() + within;
    while Instant::now() < end {
        match events.recv_timeout(end.saturating_duration_since(Instant::now())) {
            Ok(Event::Bluetooth(state)) if wanted(&state) => return Some(*state),
            Ok(Event::Request(_, client)) => client.reply(y2p_contract::radiod::Reply::done()),
            Ok(_) => {}
            Err(_) => return None,
        }
    }
    None
}

/// This process's user + system CPU time in clock ticks (/proc/self/stat fields 14 and 15).
fn cpu_ticks() -> u64 {
    let stat = std::fs::read_to_string("/proc/self/stat").expect("stat");
    let fields: Vec<&str> = stat
        .rsplit_once(')')
        .map(|(_, rest)| rest.split_whitespace().collect())
        .unwrap_or_default();
    // After the command name: field 3 is index 0, so utime (14) is 11 and stime (15) is 12.
    let tick = |index: usize| {
        fields
            .get(index)
            .and_then(|f| f.parse::<u64>().ok())
            .unwrap_or(0)
    };
    tick(11) + tick(12)
}

fn fixture(name: &str) -> (Root, std::path::PathBuf) {
    let dir = std::env::temp_dir().join(format!("y2-radiod-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(dir.join("data/bluetooth")).expect("fixture");
    std::fs::write(dir.join("data/bluetooth/enabled"), "1\n").expect("fixture");
    std::fs::write(
        dir.join("data/bluetooth/preferred-audio"),
        "12:34:56:78:90:AB",
    )
    .expect("fixture");
    (Root::new(&dir), dir)
}

#[test]
fn bluetooth_comes_on_pages_the_headset_after_adoption_recovers_in_progress_and_sleeps() {
    let Some(bus) = private_bus() else {
        eprintln!("no dbus-daemon on this host: skipped");
        return;
    };
    std::env::set_var("DBUS_SYSTEM_BUS_ADDRESS", &bus.address);
    let fake = Arc::new(Mutex::new(Fake {
        in_progress_first: true,
        ..Fake::default()
    }));
    let stop = Arc::new(AtomicBool::new(false));
    let (ready, served) = mpsc::channel();
    let server = {
        let (address, fake, stop) = (bus.address.clone(), fake.clone(), stop.clone());
        std::thread::spawn(move || serve(address, fake, stop, ready))
    };
    served
        .recv_timeout(Duration::from_secs(5))
        .expect("fake up");
    let (root, dir) = fixture("e2e");
    let (events, received) = mpsc::channel();
    let started = Instant::now();
    let handle = spawn(
        &root,
        crate::journal::Journal::new(&root),
        events,
        Desired::load(&root),
    )
    .expect("controller");
    // Powered by the controller, then the headset paged; the first page meets InProgress and is
    // cancelled after STUCK, the next one connects.
    let ready = until(&received, Duration::from_secs(40), |s| {
        s.devices.iter().any(|d| d.link == Link::Ready)
    })
    .expect("headset ready");
    assert!(ready.powered && ready.bluealsa);
    assert!(
        started.elapsed() >= links::ADOPTION,
        "no page before adoption"
    );
    assert!(
        fake.lock().expect("fake").connects >= 2,
        "InProgress was retried"
    );
    assert!(ready.pcms[0].generation > 0);
    // Settled: the controller waits instead of spinning (its thread's CPU time over 2 s).
    let before = cpu_ticks();
    std::thread::sleep(Duration::from_secs(2));
    let spent = cpu_ticks() - before;
    assert!(spent < 20, "{spent} ticks of CPU in 2 s while idle");
    // A sleep: the adapter goes down and the quiesce is answered.
    let (client, peer) = std::os::unix::net::UnixStream::pair().expect("pair");
    let client = crate::server::Client::for_tests(client);
    handle.send(
        Request::Quiesce {
            radio: Radio::Bluetooth,
        },
        client,
    );
    let down = until(&received, Duration::from_secs(10), |s| !s.powered).expect("down");
    assert_eq!(down.service, y2p_contract::radiod::Service::Quiesced);
    let mut answer = String::new();
    peer.set_read_timeout(Some(Duration::from_secs(5)))
        .expect("timeout");
    BufReader::new(peer).read_line(&mut answer).expect("answer");
    assert!(answer.contains("\"ok\":true"), "{answer}");
    stop.store(true, Ordering::Release);
    let _ = server.join();
    let _ = std::fs::remove_dir_all(dir);
}
