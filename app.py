import os
import sqlite3
from datetime import datetime
from flask import Flask, request, jsonify, render_template
from flask_cors import CORS

app = Flask(__name__, template_folder="templates")
CORS(app)

DB_PATH = "attendance.db"
MAX_SLOTS = 162      # must match MAX_USERS in the ESP32 firmware
TEACHER_SLOT = 1    # the first record in the database is always the teacher


def get_conn():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def column_names(conn, table):
    """Column names of a table ([] if the table doesn't exist yet)."""
    return [r["name"] for r in conn.execute(f"PRAGMA table_info({table})").fetchall()]


def utc_now():
    return datetime.utcnow().isoformat(sep=" ", timespec="seconds")


def init_db():
    conn = get_conn()

    # ---- One-time migration from the old RFID schema -------------------
    legacy_users = "rfid_uid" in column_names(conn, "users")
    legacy_log = "rfid_uid" in column_names(conn, "attendance_log")

    if legacy_users:
        conn.execute("ALTER TABLE users RENAME TO users_old")
    if legacy_log:
        conn.execute("ALTER TABLE attendance_log RENAME TO attendance_log_old")

    conn.execute('''
        CREATE TABLE IF NOT EXISTS users (
            slot_id INTEGER PRIMARY KEY,
            fingerprint_id INTEGER NOT NULL,
            name TEXT,
            enrolled_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            visit_count INTEGER DEFAULT 0,
            last_seen DATETIME
        )
    ''')
    conn.execute('''
        CREATE TABLE IF NOT EXISTS sessions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            started_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            teacher_marked_at DATETIME
        )
    ''')
    conn.execute('''
        CREATE TABLE IF NOT EXISTS attendance_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            slot_id INTEGER,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
            source TEXT,
            session_id INTEGER
        )
    ''')

    if legacy_users:
        conn.execute('''
            INSERT INTO users (slot_id, fingerprint_id, name, enrolled_at, visit_count, last_seen)
            SELECT slot_id, COALESCE(fingerprint_id, slot_id), name, enrolled_at, visit_count, last_seen
            FROM users_old
        ''')
        conn.execute("DROP TABLE users_old")
    if legacy_log:
        conn.execute('''
            INSERT INTO attendance_log (id, slot_id, timestamp, source)
            SELECT id, slot_id, timestamp, source FROM attendance_log_old
        ''')
        conn.execute("DROP TABLE attendance_log_old")

    # ---- Add session_id to a database created before sessions existed --
    if "session_id" not in column_names(conn, "attendance_log"):
        conn.execute("ALTER TABLE attendance_log ADD COLUMN session_id INTEGER")

    # One mark per user per session, enforced by the database itself.
    # (Old rows with session_id NULL are ignored by this index.)
    conn.execute(
        "CREATE UNIQUE INDEX IF NOT EXISTS ux_attendance_session_slot "
        "ON attendance_log(session_id, slot_id)"
    )

    # There is always a current session: create the first one if none exist.
    if conn.execute("SELECT COUNT(*) AS c FROM sessions").fetchone()["c"] == 0:
        conn.execute("INSERT INTO sessions (started_at) VALUES (?)", (utc_now(),))

    conn.commit()
    conn.close()


init_db()


# =====================================================================
# Helpers
# =====================================================================
def user_row_to_dict(row):
    return {
        "slot_id": row["slot_id"],
        "role": "teacher" if row["slot_id"] == TEACHER_SLOT else "student",
        "fingerprint_id": row["fingerprint_id"],
        "name": row["name"],
        "enrolled_at": row["enrolled_at"],
        "visit_count": row["visit_count"],
        "last_seen": row["last_seen"],
    }


def parse_slot_id(value):
    """Return an int slot id within 1..MAX_SLOTS, or None if invalid."""
    try:
        slot = int(value)
    except (TypeError, ValueError):
        return None
    return slot if 1 <= slot <= MAX_SLOTS else None


def get_current_session(conn):
    """The current session is simply the most recently created one."""
    return conn.execute("SELECT * FROM sessions ORDER BY id DESC LIMIT 1").fetchone()


def session_to_dict(conn, session):
    marked = [
        r["slot_id"] for r in conn.execute(
            "SELECT slot_id FROM attendance_log WHERE session_id = ? ORDER BY id ASC",
            (session["id"],),
        ).fetchall()
    ]
    return {
        "id": session["id"],
        "started_at": session["started_at"],
        "teacher_marked": session["teacher_marked_at"] is not None,
        "teacher_marked_at": session["teacher_marked_at"],
        "students_present": len([s for s in marked if s != TEACHER_SLOT]),
        "marked_slots": marked,
    }


def error(message, status, code=None):
    body = {"success": False, "error": message}
    if code:
        body["code"] = code
    return jsonify(body), status


# =====================================================================
# Routes
# =====================================================================
@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/health", methods=["GET"])
def health():
    conn = get_conn()
    count = conn.execute("SELECT COUNT(*) AS c FROM users").fetchone()["c"]
    conn.close()
    return jsonify({
        "status": "online",
        "database": os.path.exists(DB_PATH),
        "enrolled_users": count,
        "max_slots": MAX_SLOTS,
        "teacher_slot": TEACHER_SLOT,
    })


# ---- Users / enrollment sync ------------------------------------------
@app.route("/api/users", methods=["GET"])
def get_users():
    """Full user list. The ESP32 calls this once on boot to rebuild its
    in-memory userEnrolled[] / attendanceCount[] arrays, and the dashboard
    renders its table from it. Slot 1 is the teacher (role = "teacher")."""
    conn = get_conn()
    rows = conn.execute("SELECT * FROM users ORDER BY slot_id ASC").fetchall()
    conn.close()
    if request.args.get("compact"):
        return jsonify({"success": True, "users": [
            {"slot_id": r["slot_id"], "visit_count": r["visit_count"]} for r in rows]})
    return jsonify({
        "success": True,
        "max_slots": MAX_SLOTS,
        "teacher_slot": TEACHER_SLOT,
        "users": [user_row_to_dict(r) for r in rows],
    })


@app.route("/api/users", methods=["POST"])
def enroll_user():
    """Called by the ESP32 right after a fingerprint enrollment succeeds.

    Body: { "slot_id": 1, "fingerprint_id": 1 }
    The teacher (slot 1) must be enrolled before any student.
    """
    data = request.get_json(silent=True) or {}
    slot_id = parse_slot_id(data.get("slot_id"))
    name = data.get("name")  # optional - dashboard fills names in later

    if slot_id is None:
        return error(f"slot_id is required and must be between 1 and {MAX_SLOTS}", 400)

    fingerprint_id = data.get("fingerprint_id", slot_id)
    try:
        fingerprint_id = int(fingerprint_id)
    except (TypeError, ValueError):
        return error("fingerprint_id must be an integer", 400)

    conn = get_conn()

    if slot_id != TEACHER_SLOT:
        teacher = conn.execute(
            "SELECT 1 FROM users WHERE slot_id = ?", (TEACHER_SLOT,)
        ).fetchone()
        if not teacher:
            conn.close()
            return error("The teacher must be enrolled first", 409, "teacher_not_enrolled")

    try:
        # Re-enrolling an existing slot keeps its name, visit_count and history.
        conn.execute(
            "INSERT INTO users (slot_id, fingerprint_id, name) VALUES (?, ?, ?) "
            "ON CONFLICT(slot_id) DO UPDATE SET fingerprint_id=excluded.fingerprint_id",
            (slot_id, fingerprint_id, name),
        )
        conn.commit()
    except sqlite3.IntegrityError as e:
        conn.close()
        return error(f"Enrollment conflict: {e}", 409)

    row = conn.execute("SELECT * FROM users WHERE slot_id = ?", (slot_id,)).fetchone()
    conn.close()
    return jsonify({"success": True, "user": user_row_to_dict(row)})


@app.route("/api/users/<int:slot_id>", methods=["PATCH"])
def update_user(slot_id):
    """Dashboard-only: attach or edit a human-readable name for a slot."""
    data = request.get_json(silent=True) or {}
    name = data.get("name")
    if name is None:
        return error("name is required", 400)

    conn = get_conn()
    cur = conn.execute("UPDATE users SET name = ? WHERE slot_id = ?", (name, slot_id))
    conn.commit()
    if cur.rowcount == 0:
        conn.close()
        return error("No such slot_id", 404)
    row = conn.execute("SELECT * FROM users WHERE slot_id = ?", (slot_id,)).fetchone()
    conn.close()
    return jsonify({"success": True, "user": user_row_to_dict(row)})


@app.route("/api/users/<int:slot_id>", methods=["DELETE"])
def delete_user(slot_id):
    """Frees a slot server-side (dashboard 'remove user' action).

    The teacher can't be removed while students are still enrolled, since
    students depend on the teacher's attendance mark.

    This does NOT erase the fingerprint template on the sensor's own
    flash - call finger.deleteModel(slot_id) from the firmware for that.
    """
    conn = get_conn()

    if slot_id == TEACHER_SLOT:
        others = conn.execute(
            "SELECT COUNT(*) AS c FROM users WHERE slot_id != ?", (TEACHER_SLOT,)
        ).fetchone()["c"]
        if others > 0:
            conn.close()
            return error("Remove all students before removing the teacher", 409, "teacher_has_students")

    cur = conn.execute("DELETE FROM users WHERE slot_id = ?", (slot_id,))
    conn.commit()
    conn.close()
    if cur.rowcount == 0:
        return error("No such slot_id", 404)
    return jsonify({"success": True})


# ---- Class sessions -------------------------------------------------------
@app.route("/api/session", methods=["GET"])
def get_session():
    """Current class session. The ESP32 reads this on boot so it knows
    whether the teacher has already marked and who is already present."""
    conn = get_conn()
    session = get_current_session(conn)
    result = session_to_dict(conn, session)
    conn.close()
    return jsonify({"success": True, "session": result, "marked_slots": result["marked_slots"]})


@app.route("/api/session/new", methods=["POST"])
def new_session():
    """Starts a new class session (ESP32 calls this after the button is
    pressed 3 times). Everyone is unmarked again and the teacher must
    mark first.

    If the current session has no attendance at all yet, it is reused
    instead of piling up empty sessions (this also makes retries safe)."""
    conn = get_conn()
    current = get_current_session(conn)
    used = conn.execute(
        "SELECT COUNT(*) AS c FROM attendance_log WHERE session_id = ?", (current["id"],)
    ).fetchone()["c"]

    reused = used == 0
    if not reused:
        conn.execute("INSERT INTO sessions (started_at) VALUES (?)", (utc_now(),))
        conn.commit()
        current = get_current_session(conn)

    result = session_to_dict(conn, current)
    conn.close()
    return jsonify({"success": True, "reused": reused, "session": result})


@app.route("/api/sessions", methods=["GET"])
def list_sessions():
    limit = request.args.get("limit", default=20, type=int)
    limit = max(1, min(limit, 100))

    conn = get_conn()
    rows = conn.execute('''
        SELECT s.id, s.started_at, s.teacher_marked_at,
               (SELECT COUNT(*) FROM attendance_log a
                WHERE a.session_id = s.id AND a.slot_id != ?) AS students_present
        FROM sessions s ORDER BY s.id DESC LIMIT ?
    ''', (TEACHER_SLOT, limit)).fetchall()
    conn.close()

    return jsonify({"success": True, "sessions": [{
        "id": r["id"],
        "started_at": r["started_at"],
        "teacher_marked": r["teacher_marked_at"] is not None,
        "teacher_marked_at": r["teacher_marked_at"],
        "students_present": r["students_present"],
    } for r in rows]})


# ---- Attendance ---------------------------------------------------------
@app.route("/api/attendance", methods=["POST"])
def log_attendance():
    """Called by the ESP32 on every successful fingerprint match.
    Body: { "slot_id": 1, "fingerprint_id": 1, "source": "ESP32" }.

    Rules for the current session:
      - the teacher (slot 1) marks first, once;
      - students are rejected (403, code teacher_not_marked) until then;
      - each student can mark only once (409, code already_marked).
    """
    data = request.get_json(silent=True) or {}
    slot_id = parse_slot_id(data.get("slot_id"))
    source = data.get("source", "ESP32")

    if slot_id is None:
        return error(f"slot_id is required and must be between 1 and {MAX_SLOTS}", 400)

    conn = get_conn()
    user = conn.execute("SELECT * FROM users WHERE slot_id = ?", (slot_id,)).fetchone()
    if not user:
        conn.close()
        return error("Unknown slot_id - enroll first", 404, "unknown_slot")

    session = get_current_session(conn)
    is_teacher = slot_id == TEACHER_SLOT

    if is_teacher and session["teacher_marked_at"]:
        conn.close()
        return error("Teacher already marked for this session", 409, "already_marked")
    if not is_teacher and not session["teacher_marked_at"]:
        conn.close()
        return error("The teacher has not marked attendance yet", 403, "teacher_not_marked")

    now = utc_now()
    try:
        conn.execute(
            "INSERT INTO attendance_log (slot_id, source, timestamp, session_id) VALUES (?, ?, ?, ?)",
            (slot_id, source, now, session["id"]),
        )
    except sqlite3.IntegrityError:
        conn.rollback()
        conn.close()
        return error("Already marked for this session", 409, "already_marked")

    conn.execute(
        "UPDATE users SET visit_count = visit_count + 1, last_seen = ? WHERE slot_id = ?",
        (now, slot_id),
    )
    if is_teacher:
        conn.execute(
            "UPDATE sessions SET teacher_marked_at = ? WHERE id = ?", (now, session["id"])
        )
    conn.commit()

    row = conn.execute("SELECT * FROM users WHERE slot_id = ?", (slot_id,)).fetchone()
    result = session_to_dict(conn, get_current_session(conn))
    conn.close()

    return jsonify({"success": True, "user": user_row_to_dict(row), "session": result})


@app.route("/api/attendance", methods=["GET"])
def get_attendance():
    """Attendance log, newest first. Optional ?session_id=N to see one session."""
    limit = request.args.get("limit", default=25, type=int)
    limit = max(1, min(limit, 100))
    session_id = request.args.get("session_id", type=int)

    query = '''
        SELECT a.id, a.slot_id, a.timestamp, a.source, a.session_id, u.name
        FROM attendance_log a
        LEFT JOIN users u ON u.slot_id = a.slot_id
    '''
    params = []
    if session_id is not None:
        query += " WHERE a.session_id = ?"
        params.append(session_id)
    query += " ORDER BY a.id DESC LIMIT ?"
    params.append(limit)

    conn = get_conn()
    rows = conn.execute(query, params).fetchall()
    conn.close()

    log = [{
        "id": r["id"],
        "slot_id": r["slot_id"],
        "role": "teacher" if r["slot_id"] == TEACHER_SLOT else "student",
        "session_id": r["session_id"],
        "timestamp": r["timestamp"],
        "source": r["source"],
        "name": r["name"],
    } for r in rows]

    return jsonify({"success": True, "log": log})


@app.route("/api/stats", methods=["GET"])
def get_stats():
    conn = get_conn()
    total_users = conn.execute("SELECT COUNT(*) AS c FROM users").fetchone()["c"]
    total_scans = conn.execute("SELECT COUNT(*) AS c FROM attendance_log").fetchone()["c"]
    today_scans = conn.execute(
        "SELECT COUNT(*) AS c FROM attendance_log WHERE date(timestamp) = date('now')"
    ).fetchone()["c"]
    last_scan_row = conn.execute('''
        SELECT a.timestamp, u.name, a.slot_id FROM attendance_log a
        LEFT JOIN users u ON u.slot_id = a.slot_id
        ORDER BY a.id DESC LIMIT 1
    ''').fetchone()
    current_session = session_to_dict(conn, get_current_session(conn))
    total_sessions = conn.execute("SELECT COUNT(*) AS c FROM sessions").fetchone()["c"]
    conn.close()

    return jsonify({
        "success": True,
        "total_users": total_users,
        "max_slots": MAX_SLOTS,
        "free_slots": MAX_SLOTS - total_users,
        "total_scans": total_scans,
        "today_scans": today_scans,
        "total_sessions": total_sessions,
        "current_session": current_session,
        "last_scan": {
            "timestamp": last_scan_row["timestamp"],
            "name": last_scan_row["name"],
            "slot_id": last_scan_row["slot_id"],
        } if last_scan_row else None,
    })


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)
