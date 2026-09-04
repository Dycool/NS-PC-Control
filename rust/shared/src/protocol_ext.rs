use crate::protocol::HidReport;

impl HidReport {
    /// Mark this report as representing a physically present logical pad.
    ///
    /// The wire bit lives inside the nested eight-byte input report. Keeping
    /// this mutation here preserves `HidReport`'s field encapsulation for
    /// server/client crates.
    pub fn set_pad_present(&mut self, present: bool) {
        self.input.set_pad_present(present);
    }
}
