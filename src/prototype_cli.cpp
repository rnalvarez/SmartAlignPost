            << p.confidence << ","
            << (p.keyPoint ? 1 : 0) << ","
            << phatMs << ","
            << waveformMs << ","
            << p.phaseAgreement
            << "\n";
    }

    return 0;
}