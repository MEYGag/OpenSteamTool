#pragma once

namespace ManifestUpdateScheduler {

    // Starts the background periodic update scheduler if check_interval > 0 and GitHub manifest repo is configured.
    void Start();

    // Stops the background periodic update scheduler.
    void Stop();

} // namespace ManifestUpdateScheduler
