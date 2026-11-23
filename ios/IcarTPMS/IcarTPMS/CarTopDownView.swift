import SwiftUI

struct ChassisShape: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        let w = rect.width
        let h = rect.height

        // Top center (front bumper)
        path.move(to: CGPoint(x: w * 0.5, y: 0))

        // Front right curve
        path.addQuadCurve(to: CGPoint(x: w * 0.88, y: h * 0.08), control: CGPoint(x: w * 0.78, y: 0))
        // Front right wheel arch
        path.addCurve(to: CGPoint(x: w * 0.94, y: h * 0.28),
                      control1: CGPoint(x: w * 0.96, y: h * 0.14),
                      control2: CGPoint(x: w * 0.98, y: h * 0.22))
        // Right door pinch / waist
        path.addCurve(to: CGPoint(x: w * 0.90, y: h * 0.65),
                      control1: CGPoint(x: w * 0.86, y: h * 0.38),
                      control2: CGPoint(x: w * 0.86, y: h * 0.55))
        // Rear right wheel arch
        path.addCurve(to: CGPoint(x: w * 0.96, y: h * 0.84),
                      control1: CGPoint(x: w * 0.96, y: h * 0.70),
                      control2: CGPoint(x: w * 0.98, y: h * 0.78))
        // Rear right bumper
        path.addQuadCurve(to: CGPoint(x: w * 0.5, y: h), control: CGPoint(x: w * 0.85, y: h * 0.98))

        // Rear left bumper
        path.addQuadCurve(to: CGPoint(x: w * 0.04, y: h * 0.84), control: CGPoint(x: w * 0.15, y: h * 0.98))
        // Rear left wheel arch
        path.addCurve(to: CGPoint(x: w * 0.10, y: h * 0.65),
                      control1: CGPoint(x: w * 0.02, y: h * 0.78),
                      control2: CGPoint(x: w * 0.04, y: h * 0.70))
        // Left door pinch / waist
        path.addCurve(to: CGPoint(x: w * 0.06, y: h * 0.28),
                      control1: CGPoint(x: w * 0.14, y: h * 0.55),
                      control2: CGPoint(x: w * 0.14, y: h * 0.38))
        // Front left wheel arch
        path.addCurve(to: CGPoint(x: w * 0.12, y: h * 0.08),
                      control1: CGPoint(x: w * 0.02, y: h * 0.22),
                      control2: CGPoint(x: w * 0.04, y: h * 0.14))
        // Front left curve to center
        path.addQuadCurve(to: CGPoint(x: w * 0.5, y: 0), control: CGPoint(x: w * 0.22, y: 0))

        path.closeSubpath()
        return path
    }
}

struct WindshieldShape: Shape {
    let curvedDown: Bool

    func path(in rect: CGRect) -> Path {
        var path = Path()
        let w = rect.width
        let h = rect.height

        if curvedDown {
            path.move(to: CGPoint(x: w * 0.12, y: h))
            path.addQuadCurve(to: CGPoint(x: w * 0.88, y: h), control: CGPoint(x: w * 0.5, y: 0))
            path.addLine(to: CGPoint(x: w * 0.80, y: 0))
            path.addQuadCurve(to: CGPoint(x: w * 0.20, y: 0), control: CGPoint(x: w * 0.5, y: -h * 0.3))
            path.closeSubpath()
        } else {
            path.move(to: CGPoint(x: w * 0.18, y: 0))
            path.addQuadCurve(to: CGPoint(x: w * 0.82, y: 0), control: CGPoint(x: w * 0.5, y: h * 0.5))
            path.addLine(to: CGPoint(x: w * 0.88, y: h))
            path.addQuadCurve(to: CGPoint(x: w * 0.12, y: h), control: CGPoint(x: w * 0.5, y: h * 1.3))
            path.closeSubpath()
        }
        return path
    }
}

struct MirrorShape: Shape {
    let isLeft: Bool

    func path(in rect: CGRect) -> Path {
        var path = Path()
        let w = rect.width
        let h = rect.height
        if isLeft {
            path.move(to: CGPoint(x: w, y: h * 0.4))
            path.addQuadCurve(to: CGPoint(x: 0, y: 0), control: CGPoint(x: 0, y: h * 0.2))
            path.addQuadCurve(to: CGPoint(x: w, y: h), control: CGPoint(x: 0, y: h * 0.8))
        } else {
            path.move(to: CGPoint(x: 0, y: h * 0.4))
            path.addQuadCurve(to: CGPoint(x: w, y: 0), control: CGPoint(x: w, y: h * 0.2))
            path.addQuadCurve(to: CGPoint(x: 0, y: h), control: CGPoint(x: w, y: h * 0.8))
        }
        path.closeSubpath()
        return path
    }
}

struct MiniTirePad: View {
    let reading: TireReading?

    private var padColor: Color {
        guard let reading else { return Color.gray.opacity(0.35) }
        switch reading.healthStatus {
        case .normal: return Color.green
        case .lowPressure, .highPressure, .highTemperature: return Color.orange
        case .fault: return Color.red
        }
    }

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 4)
                .fill(padColor.opacity(reading == nil ? 0.08 : 0.4))
                .blur(radius: reading == nil ? 1 : 4)
                .frame(width: 15, height: 32)

            RoundedRectangle(cornerRadius: 3.5)
                .fill(
                    LinearGradient(
                        colors: [Color(white: 0.25), Color(white: 0.12)],
                        startPoint: .leading,
                        endPoint: .trailing
                    )
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 3.5)
                        .stroke(padColor, lineWidth: reading == nil ? 1 : 1.8)
                )
                .frame(width: 12, height: 28)
        }
    }
}

struct CarTopDownView: View {
    var wheels: [TireReading?] = Array(repeating: nil, count: 4)

    var body: some View {
        ZStack {
            // Subtle ambient backdrop glow
            RoundedRectangle(cornerRadius: 36)
                .fill(
                    RadialGradient(
                        colors: [Color.blue.opacity(0.08), Color.clear],
                        center: .center,
                        startRadius: 20,
                        endRadius: 110
                    )
                )
                .frame(width: 130, height: 250)

            // 4 Mini Tires attached to chassis
            VStack {
                // Front axle (FL, FR)
                HStack {
                    MiniTirePad(reading: wheels[0])
                    Spacer()
                    MiniTirePad(reading: wheels[1])
                }
                .padding(.horizontal, 2)

                Spacer()

                // Rear axle (RL, RR)
                HStack {
                    MiniTirePad(reading: wheels[2])
                    Spacer()
                    MiniTirePad(reading: wheels[3])
                }
                .padding(.horizontal, 2)
            }
            .frame(width: 126, height: 160)

            // Vehicle silhouette body
            ChassisShape()
                .fill(
                    LinearGradient(
                        colors: [
                            Color(white: 0.22),
                            Color(white: 0.16),
                            Color(white: 0.12)
                        ],
                        startPoint: .top,
                        endPoint: .bottom
                    )
                )
                .overlay(
                    ChassisShape()
                        .stroke(
                            LinearGradient(
                                colors: [Color.white.opacity(0.35), Color.white.opacity(0.1)],
                                startPoint: .top,
                                endPoint: .bottom
                            ),
                            lineWidth: 1.2
                        )
                )
                .frame(width: 98, height: 216)
                .shadow(color: Color.black.opacity(0.35), radius: 10, x: 0, y: 6)

            // Cabin elements: Windshield, Sunroof, Rear glass
            VStack(spacing: 6) {
                // Front windshield
                WindshieldShape(curvedDown: true)
                    .fill(Color.cyan.opacity(0.18))
                    .overlay(WindshieldShape(curvedDown: true).stroke(Color.cyan.opacity(0.35), lineWidth: 1))
                    .frame(width: 62, height: 24)
                    .padding(.top, 42)

                // Sunroof / Roofline
                RoundedRectangle(cornerRadius: 6)
                    .fill(Color.white.opacity(0.06))
                    .overlay(RoundedRectangle(cornerRadius: 6).stroke(Color.white.opacity(0.12), lineWidth: 0.8))
                    .frame(width: 54, height: 44)

                // Rear windshield
                WindshieldShape(curvedDown: false)
                    .fill(Color.cyan.opacity(0.14))
                    .overlay(WindshieldShape(curvedDown: false).stroke(Color.cyan.opacity(0.28), lineWidth: 1))
                    .frame(width: 60, height: 20)

                Spacer(minLength: 0)
            }
            .frame(width: 98, height: 216)

            // Headlights & Taillights
            VStack {
                HStack {
                    Capsule().fill(Color.cyan.opacity(0.9)).frame(width: 14, height: 4)
                    Spacer()
                    Capsule().fill(Color.cyan.opacity(0.9)).frame(width: 14, height: 4)
                }
                .padding(.horizontal, 14)
                .padding(.top, 4)

                Spacer()

                HStack {
                    Capsule().fill(Color.red.opacity(0.9)).frame(width: 14, height: 3)
                    Spacer()
                    Capsule().fill(Color.red.opacity(0.9)).frame(width: 14, height: 3)
                }
                .padding(.horizontal, 14)
                .padding(.bottom, 4)
            }
            .frame(width: 98, height: 216)

            // Side mirrors
            HStack {
                MirrorShape(isLeft: true).fill(Color(white: 0.25)).frame(width: 8, height: 16)
                Spacer()
                MirrorShape(isLeft: false).fill(Color(white: 0.25)).frame(width: 8, height: 16)
            }
            .frame(width: 114)
            .offset(y: -42)
        }
        .frame(width: 130, height: 250)
    }
}
