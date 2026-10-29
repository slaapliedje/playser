pragma Singleton
import QtQuick
import QtCore

// Where each disc was left: { index (queue entry, e.g. episode), position
// (seconds), title }, keyed by the disc's content hash (or its label).
QtObject {
    id: root

    property Settings store: Settings {
        category: "resume"
        property string discs: "{}"
    }

    function all() {
        try { return JSON.parse(store.discs) } catch (e) { return {} }
    }

    function get(key) {
        return key ? all()[key] || null : null
    }

    function save(key, entry) {
        if (!key)
            return
        const d = all()
        d[key] = Object.assign({ saved: Date.now() }, entry)
        store.discs = JSON.stringify(d)
    }

    function clear(key) {
        const d = all()
        if (key in d) {
            delete d[key]
            store.discs = JSON.stringify(d)
        }
    }
}
