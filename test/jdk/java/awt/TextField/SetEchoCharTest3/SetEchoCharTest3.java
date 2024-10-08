/*
 * Copyright (c) 1999, 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/*
 * @test
 * @bug 4222122
 * @summary TextField.setEchoCharacter() seems to be broken
 * @library /java/awt/regtesthelpers
 * @build PassFailJFrame
 * @run main/manual SetEchoCharTest3
 */

import java.awt.FlowLayout;
import java.awt.Frame;
import java.awt.Label;
import java.awt.TextField;
import java.lang.reflect.InvocationTargetException;

public class SetEchoCharTest3 extends Frame {
    static String INSTRUCTIONS = """
             Type in the text field and "*" characters should echo.
             If only one "*" echoes and then the system beeps after
             the second character is typed, then press Fail, otherwise press Pass.
             """;
    public SetEchoCharTest3() {
        setLayout(new FlowLayout());
        add(new Label("Enter text:"));
        TextField tf = new TextField(15);
        tf.setEchoChar('*');
        add(tf);
        pack();
    }

    public static void main(String[] args) throws InterruptedException,
            InvocationTargetException {
        PassFailJFrame.builder()
                .title("Set Echo Char Test 3")
                .testUI(SetEchoCharTest3::new)
                .instructions(INSTRUCTIONS)
                .rows((int) INSTRUCTIONS.lines().count() + 1)
                .columns(40)
                .build()
                .awaitAndCheck();
    }
}
